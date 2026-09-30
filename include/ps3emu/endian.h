/*
 * ps3recomp - Big-endian conversion utilities
 *
 * Byte swap helpers for translating between PS3 (big-endian) and host
 * (little-endian) byte order.
 */

#ifndef PS3EMU_ENDIAN_H
#define PS3EMU_ENDIAN_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Platform intrinsics
 * -----------------------------------------------------------------------*/

#if defined(_MSC_VER)
  #include <stdlib.h>   /* _byteswap_* */

  static inline uint16_t ps3_bswap16(uint16_t v) { return _byteswap_ushort(v); }
  static inline uint32_t ps3_bswap32(uint32_t v) { return _byteswap_ulong(v);  }
  static inline uint64_t ps3_bswap64(uint64_t v) { return _byteswap_uint64(v); }

#elif defined(__GNUC__) || defined(__clang__)

  static inline uint16_t ps3_bswap16(uint16_t v) { return __builtin_bswap16(v); }
  static inline uint32_t ps3_bswap32(uint32_t v) { return __builtin_bswap32(v); }
  static inline uint64_t ps3_bswap64(uint64_t v) { return __builtin_bswap64(v); }

#else
  /* Portable fallbacks */
  static inline uint16_t ps3_bswap16(uint16_t v)
  {
      return (uint16_t)((v >> 8) | (v << 8));
  }

  static inline uint32_t ps3_bswap32(uint32_t v)
  {
      v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
      return (v << 16) | (v >> 16);
  }

  static inline uint64_t ps3_bswap64(uint64_t v)
  {
      v = ((v & 0x00FF00FF00FF00FFull) <<  8) | ((v & 0xFF00FF00FF00FF00ull) >>  8);
      v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v & 0xFFFF0000FFFF0000ull) >> 16);
      return (v << 32) | (v >> 32);
  }
#endif

#endif /* PS3EMU_ENDIAN_H */
