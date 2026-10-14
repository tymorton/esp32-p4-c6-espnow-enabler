#pragma once
#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#ifndef TEST_IDF_MINOR
#define TEST_IDF_MINOR 5
#endif
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, TEST_IDF_MINOR, 1)
