/**
 * @file TypeDefine.h
 * @brief 자료형 정의 헤더 파일
*/
#ifndef __TYPEDEFINE_H__
#define __TYPEDEFINE_H__

#include <inttypes.h>

typedef int8_t					sint8;
typedef int16_t					sint16;
typedef int32_t					sint32;
typedef int64_t					sint64;

typedef unsigned char			byte;
typedef uint8_t					uint8;
typedef uint16_t				uint16;
typedef uint32_t				uint32;
typedef uint64_t				uint64;

#define SINT8_SIZE				sizeof(sint8)
#define SINT16_SIZE				sizeof(sint16)
#define SINT32_SIZE				sizeof(sint32)
#define SINT64_SIZE				sizeof(sint64)

#define BYTE_SIZE				sizeof(byte)
#define UINT8_SIZE				sizeof(uint8)
#define UINT16_SIZE				sizeof(uint16)
#define UINT32_SIZE				sizeof(uint32)
#define UINT64_SIZE				sizeof(uint64)

#define SHORT_SIZE				sizeof(short)
#define INT_SIZE				sizeof(int)
#define FLOAT_SIZE				sizeof(float)
#define DOUBLE_SIZE				sizeof(double)

#endif	// __TYPEDEFINE_H__
