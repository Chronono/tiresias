/*
 * fp_handler.c
 *
 *  Created on: Jun 10, 2018
 *      Author: pchero
 */

#define _GNU_SOURCE

//  #define DEBUG_HANDLER

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <aubio/aubio.h>
#include <math.h>
#include <jansson.h>
#include <openssl/md5.h>
#include <libgen.h>

#include <byteswap.h>

#include "slog.h"
#include "db_ctx_handler.h"
#include "fp_handler.h"
#include "utils.h"

// MES PROPRES MODIFS:
#include "aubio_source_raw.h"
#include "../../aubio/aubio-0.4.9/src/aubio_priv.h"


db_ctx_t* g_db_ctx;

#define DEF_SEARCH_TOLERANCE		0.001

#define DEF_DATABASE_NAME			":memory:"
#define DEF_BACKUP_DATABASE			"audio_recongition.db"
#define DEF_WINDOW_SIZE_CONSTANCE	100

#define DEF_AUBIO_HOPSIZE		256
#define DEF_AUBIO_BUFSIZE		512
#define DEF_AUBIO_SAMPLERATE	0		// read samplerate from the file
#define DEF_AUBIO_FILTER		40
#define DEF_AUBIO_COEFS			13



//#define CUSTOM_SOURCE
#define WAV_SOURCE


static bool init_database(void);

//  static bool create_audio_list_info(const char* filename, const char* uuid);
//  static bool create_audio_fingerprint_info(const char* filename, const char* uuid);
static json_t* create_audio_fingerprints(handler_audioBuffer_t * handler_buffer, const char* uuid);
//static json_t* create_audio_fingerprints_from_raw_data(const audioBuffer_t * buf, const char* uuid);

static json_t* get_audio_list_info(const char* uuid);
//  static json_t* get_audio_list_info_by_hash(const char* hash);
//  static char* create_file_hash(const char* filename);

static bool create_temp_search_table(const char* tablename);
static bool delete_temp_search_table(const char* tablename);

bool fp_init(void)
{
	int ret;

	ret = init_database();
	if(ret == false) {
		slog(LOG_ERR, "Could not initiate database.");
		return false;
	}

	// load the data into memory
	ret = db_ctx_load_db_data(g_db_ctx, DEF_BACKUP_DATABASE);
	if(ret == false) {
		slog(LOG_ERR, "Could not load the database data.");
		return false;
	}

	return true;
}

bool fp_term(void)
{
	int ret;

	ret = db_ctx_backup(g_db_ctx, DEF_BACKUP_DATABASE);
	if(ret == false) {
		slog(LOG_ERR, "Could not write database.");
		return false;
	}

	db_ctx_term(g_db_ctx);

	return true;
}


json_t* fp_search_fingerprint_info(handler_audioBuffer_t * handler_buffer)
{
	int ret;
	char* uuid = NULL;
	char* sql = NULL;
	char* tmp = NULL;
	char* tmp_max = NULL;
	char* tablename;
	json_t* j_fprints;
	json_t* j_tmp;
	json_t* j_search;
	json_t* j_res;
	int idx;
	int frame_count;
	int i;
	float tolerance = 0.0;

	if(handler_buffer == NULL) {
		slog(LOG_WARNING, "Wrong input parameter.");
		return NULL;
	}

	uuid = utils_gen_uuid();

	// create tablename
	tmp = utils_string_replace_char(uuid, '-', '_');
	asprintf(&tablename, "temp_%s", tmp);
	sfree(tmp);

	// create tmp search table
	ret = create_temp_search_table(tablename);
	if(ret == false) {
		slog(LOG_WARNING, "Could not create temp search table. tablename[%s]", tablename);
		sfree(tablename);
		sfree(uuid);
		return NULL;
	}

	// create fingerprint info
	j_fprints = create_audio_fingerprints(handler_buffer, uuid);



	sfree(uuid);
	if(j_fprints == NULL) {
		slog(LOG_ERR, "Could not create fingerprint info.");
		delete_temp_search_table(tablename);
		sfree(tablename);
		return NULL;
	}

	tolerance = (handler_buffer->conf.tolerance == 0) ? DEF_SEARCH_TOLERANCE : handler_buffer->conf.tolerance;

	// search
	frame_count = json_array_size(j_fprints);
	json_array_foreach(j_fprints, idx, j_tmp) {
		
		asprintf(&sql, "insert into %s select * from audio_fingerprint where", tablename);
		for (i = 0 ; i < handler_buffer->conf.nb_coef ; i++)
		{
			asprintf(&tmp_max, "max%u", handler_buffer->conf.mfcc_coefs[i]);	

			asprintf(&tmp, "%s (%s >= %f and %s <= %f)",
					 sql,
					 tmp_max,
					 json_real_value(json_object_get(j_tmp, tmp_max)) - tolerance,
					 tmp_max,
					 json_real_value(json_object_get(j_tmp, tmp_max)) + tolerance);

			if (i < handler_buffer->conf.nb_coef-1)
				asprintf(&tmp, "%s or", tmp);

			sfree(tmp_max);
			sfree(sql);
			sql = tmp;
		}

		asprintf(&tmp, "%s group by audio_uuid", sql);
		sfree(sql);
		sql = tmp;


		db_ctx_exec(g_db_ctx, sql);
		sfree(sql);

	}
	json_decref(j_fprints);

	// get result
	asprintf(&sql, "select *, count(*) from %s group by audio_uuid order by count(*) DESC", tablename);
	db_ctx_query(g_db_ctx, sql);
	sfree(sql);

	j_search = db_ctx_get_record(g_db_ctx);
	db_ctx_free(g_db_ctx);

	// delete temp search table
	ret = delete_temp_search_table(tablename);
	if(ret == false) {
		slog(LOG_WARNING, "Could not delete temp search table. tablename[%s]", tablename);
		sfree(tablename);
		json_decref(j_search);
		return false;
	}
	sfree(tablename);

	if(j_search == NULL) {
		// not found
		slog(LOG_NOTICE, "Could not find data.");
		return NULL;
	}

	// create result
	j_res = get_audio_list_info(json_string_value(json_object_get(j_search, "audio_uuid")));
	if(j_res == NULL) {
		slog(LOG_WARNING, "Could not find audio list info.");
		json_decref(j_search);
		return NULL;
	}

	json_object_set_new(j_res, "frame_count", json_integer(frame_count));
	json_object_set(j_res, "match_count", json_object_get(j_search, "count(*)"));
	json_decref(j_search);

	return j_res;
}

/**
 * Returns all list of fingerprinted info.
 * @return
 */
json_t* fp_get_fingerprint_lists_all(void)
{
	char* sql;
	json_t* j_res;
	json_t* j_tmp;

	// get result
	asprintf(&sql, "select * from audio_list;");
	db_ctx_query(g_db_ctx, sql);
	sfree(sql);

	j_res = json_array();
	while(1) {
		j_tmp = db_ctx_get_record(g_db_ctx);
		if(j_tmp == NULL) {
			break;
		}

		json_array_append_new(j_res, j_tmp);
	}
	db_ctx_free(g_db_ctx);

	return j_res;
}


static unsigned int copy_audioBuffer_into_regularBuffer(const handler_audioBuffer_t handler, smpl_t *buf_out)
{
	unsigned int size = 0;
	
	for (int i = 0 ; i < handler.conf.max_frames ; i++)
	{
		for (int j = 0 ; j < handler.conf.max_frame_size ; j++)
		{
			buf_out[i*handler.conf.max_frame_size + j] = ((smpl_t)handler.buffer.frames[i].data_time[j])/32767;
		}
		size += handler.conf.max_frame_size;
	}
	return size;
}

static json_t* create_audio_fingerprints(handler_audioBuffer_t* handler_buffer, const char* uuid)
{
	json_t* j_res;
	json_t* j_tmp;
	unsigned int reads  = 0;
	int count;
	int samplerate;
	int hop_size;
	char* tmp;
	int i;

	aubio_pvoc_t* pv;
	cvec_t*	fftgrain;
	aubio_mfcc_t* mfcc;
	fvec_t* mfcc_out;
	fvec_t* mfcc_buf;

	aubio_source_t* aubio_src;

	samplerate = 44100;
	hop_size = handler_buffer->conf.max_frame_size * sizeof(short); 	// FRAME_POINTS = 2048, SIZE_POINT = 2
	aubio_source_raw_data_t * aubio_raw_data = AUBIO_NEW(aubio_source_raw_data_t);

	aubio_raw_data->buffer = (smpl_t*)malloc(sizeof(smpl_t) * handler_buffer->conf.max_frames * handler_buffer->conf.max_frame_size);

	aubio_raw_data->size_buffer = copy_audioBuffer_into_regularBuffer(*handler_buffer, aubio_raw_data->buffer);
	aubio_raw_data->size_point = sizeof(smpl_t);


	aubio_raw_data->samplerate = samplerate;
	aubio_raw_data->nb_channels = 1;
	aubio_src = new_aubio_source_raw(aubio_raw_data, samplerate, hop_size);


//	sfree(source);
	if(aubio_src == NULL) {
		slog(LOG_ERR, "Could not initiate aubio src.");
		return NULL;
	}

	// initiate aubio parameters
	samplerate = aubio_source_get_samplerate(aubio_src);
	pv = new_aubio_pvoc(DEF_AUBIO_BUFSIZE, DEF_AUBIO_HOPSIZE);
	fftgrain = new_cvec(DEF_AUBIO_BUFSIZE);
	mfcc = new_aubio_mfcc(DEF_AUBIO_BUFSIZE, DEF_AUBIO_FILTER, DEF_AUBIO_COEFS, samplerate);
	mfcc_buf = new_fvec(DEF_AUBIO_HOPSIZE);
	mfcc_out = new_fvec(DEF_AUBIO_COEFS);
	if((pv == NULL) || (fftgrain == NULL) || (mfcc == NULL) || (mfcc_buf == NULL) || (mfcc_out == NULL)) {
		slog(LOG_ERR, "Could not initiate aubio parameters.");

		del_aubio_pvoc(pv);
		del_cvec(fftgrain);
		del_aubio_mfcc(mfcc);
		del_fvec(mfcc_out);
		del_fvec(mfcc_buf);
		del_aubio_source(aubio_src);
		return NULL;
	}

	j_res = json_array();
	count = 0;
	while(1) {

		aubio_source_do(aubio_src, mfcc_buf, &reads);
		if(reads == 0) {
		  break;
		}

		// compute mag spectrum
		aubio_pvoc_do(pv, mfcc_buf, fftgrain);

		// compute mfcc
		aubio_mfcc_do(mfcc, fftgrain, mfcc_out);
		
		// create mfcc data
		j_tmp = json_pack("{s:i, s:s}",
				"frame_idx",	count,
				"audio_uuid",	uuid
				);

		for(i = 0; i < DEF_AUBIO_COEFS; i++) {
			asprintf(&tmp, "max%d", i + 1);
			json_object_set_new(j_tmp, tmp, json_real(10 * log10(fabs(mfcc_out->data[i]))));
			sfree(tmp);
		}

		if(j_tmp == NULL) {
			slog(LOG_ERR, "Could not create mfcc data.");
			continue;
		}

		json_array_append_new(j_res, j_tmp);
		count++;
	}


	del_aubio_pvoc(pv);
	del_cvec(fftgrain);
	del_aubio_mfcc(mfcc);
	del_fvec(mfcc_out);
	del_fvec(mfcc_buf);
	del_aubio_source(aubio_src);

	return j_res;
}

static bool init_database(void)
{
	int ret;
	char* sql;
	char* tmp;
	int i;

	g_db_ctx = db_ctx_init(DEF_DATABASE_NAME);
	if(g_db_ctx == NULL) {
		return false;
	}

	sql = "create table audio_list("

			"   uuid        varchar(255),"
			"   name        varchar(255),"
			"	hash        varchar(1023)"
			");";
	ret = db_ctx_exec(g_db_ctx, sql);
	if(ret == false) {
		slog(LOG_ERR, "Could not create auido_list table.\n");
		return false;
	}

	// create audio_fingerprint table
	asprintf(&sql, "create table audio_fingerprint("
			" audio_uuid     varchar(255),"
			" frame_idx      integer");
	for(i = 0; i < DEF_AUBIO_COEFS; i++) {
		asprintf(&tmp, "%s, max%d real", sql, i + 1);
		sfree(sql);
		sql = tmp;
	}
	asprintf(&tmp, "%s);", sql);
	sfree(sql);
	sql = tmp;
	ret = db_ctx_exec(g_db_ctx, sql);
	sfree(sql);
	if(ret == false) {
		slog(LOG_ERR, "Could not create fingerprint table.");
		return false;
	}

	// create indices
	for(i = 1; i <= DEF_AUBIO_COEFS; i++) {
		asprintf(&sql, "create index idx_audio_fingerprint_max%d on audio_fingerprint(max%d);", i, i);
		ret = db_ctx_exec(g_db_ctx, sql);
		sfree(sql);
		if(ret == false) {
			slog(LOG_ERR, "Could not create idx_audio_fingerprint_max%d table.", i);
			return false;
		}
	}

	return true;
}



static json_t* get_audio_list_info(const char* uuid)
{
	char* sql;
	json_t* j_res;

	if(uuid == NULL) {
		slog(LOG_WARNING, "Wrong input parameter.");
		return NULL;
	}

	asprintf(&sql, "select * from audio_list where uuid = '%s';", uuid);
	db_ctx_query(g_db_ctx, sql);
	sfree(sql);

	j_res = db_ctx_get_record(g_db_ctx);
	db_ctx_free(g_db_ctx);
	if(j_res == NULL) {
		return NULL;
	}

	return j_res;
}


static bool create_temp_search_table(const char* tablename)
{
	char* sql;
	char* tmp;
	int i;
	int ret;

	if(tablename == NULL) {
		slog(LOG_WARNING, "Wrong input parameter.");
		return false;
	}



	// create audio_fingerprint table
	asprintf(&sql, "create table %s("
			" audio_uuid     varchar(255),"
			" frame_idx      integer",
			tablename
			);
	for(i = 0; i < DEF_AUBIO_COEFS; i++) {
		asprintf(&tmp, "%s, max%d real", sql, i + 1);
		sfree(sql);
		sql = tmp;
	}
	asprintf(&tmp, "%s);", sql);
	sfree(sql);
	sql = tmp;
	ret = db_ctx_exec(g_db_ctx, sql);
	sfree(sql);
	if(ret == false) {
		slog(LOG_ERR, "Could not create fingerprint search table.");
		return false;
	}

	return true;
}

static bool delete_temp_search_table(const char* tablename)
{
	char* sql;

	if(tablename == NULL) {
		slog(LOG_WARNING, "Wrong input parameter.");
		return false;
	}

	asprintf(&sql, "drop table %s;", tablename);

	db_ctx_exec(g_db_ctx, sql);
	sfree(sql);

	return true;
}

