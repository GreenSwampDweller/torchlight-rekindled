#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
void* tl_ios_view_handle(void);
void* tl_ios_controller_handle(void);
bool tl_ios_app_active(void);
void tl_ios_read_controller(float* left_x, float* left_y, float* right_x, float* right_y,
                            uint32_t* buttons, bool* connected);
const char* tl_ios_program_folder(void);
#ifdef __cplusplus
}
#endif
