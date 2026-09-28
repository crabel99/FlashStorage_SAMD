#pragma once
#include <stdint.h>
#if defined(__SAMD21__)
#include <samd21e18a.h>
#elif defined(__SAMD51__)
#include <samd51p20a.h>
#elif defined(__SAME53__)
#include <same53j19a.h>
#else
#include <same54p20a.h>
#endif
