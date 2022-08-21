/*
 * fp_handler.h
 *
 *  Created on: Jun 10, 2018
 *      Author: pchero
 */

#ifndef FP_HANDLER_H_
#define FP_HANDLER_H_


#include <stdbool.h>
#include "../../src/lib_ad.h"



bool fp_init(void);
bool fp_term(void);

json_t* fp_get_fingerprint_lists_all(void);
bool fp_delete_fingerprint_info(const char* filename);
bool fp_craete_fingerprint_info(const char* filename);
json_t* fp_search_fingerprint_info(handler_audioBuffer_t *handler_buffer);
//json_t* fp_search_fingerprint_info_from_raw_data(const audioBuffer_t buf, const int coefs);

#endif /* FP_HANDLER_H_ */
