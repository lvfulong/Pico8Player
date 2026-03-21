/* stb_image_write - v1.16 - public domain - http://nothings.org/stb
   writes out PNG/BMP/TGA/JPEG/HDR images to C stdio - Sean Barrett 2010-2015

   This is a minimal copy of stb_image_write.h, included here to avoid external
   dependencies for the p8_export tool.

   To build: in ONE .cpp file:
     #define STB_IMAGE_WRITE_IMPLEMENTATION
     #include "stb_image_write.h"
*/

#ifndef STB_IMAGE_WRITE_H
#define STB_IMAGE_WRITE_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

#ifdef __cplusplus
}
#endif

#endif // STB_IMAGE_WRITE_H

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <vector>

// This implementation only supports stbi_write_png (RGBA/RGB/Gray/GrayA).
// It is intentionally tiny for this repository.

static void stbiw__writefv(FILE *f, const char *fmt, va_list v) {
   while (*fmt) {
      switch (*fmt++) {
         case ' ': break;
         case '1': { unsigned char x = (unsigned char)va_arg(v, int); fwrite(&x,1,1,f); break; }
         case '2': { int x = va_arg(v,int); unsigned char b[2];
                     b[0]=(unsigned char)x; b[1]=(unsigned char)(x>>8);
                     fwrite(b,2,1,f); break; }
         case '4': { int x = va_arg(v,int); unsigned char b[4];
                     b[0]=(unsigned char)x; b[1]=(unsigned char)(x>>8);
                     b[2]=(unsigned char)(x>>16); b[3]=(unsigned char)(x>>24);
                     fwrite(b,4,1,f); break; }
         default: return;
      }
   }
}

static void stbiw__writef(FILE *f, const char *fmt, ...) {
   va_list v;
   va_start(v, fmt);
   stbiw__writefv(f, fmt, v);
   va_end(v);
}

// --- Minimal zlib/deflate (stored blocks) + CRC/adler ---
static unsigned int stbiw__crc32_tab[256];
static int stbiw__crc32_init = 0;

static unsigned int stbiw__crc32(unsigned int crc, const unsigned char *buffer, int len) {
   unsigned int c = crc ^ 0xffffffffu;
   if (!stbiw__crc32_init) {
      for (int i=0;i<256;i++) {
         unsigned int r = (unsigned int)i;
         for (int j=0;j<8;j++) r = (r & 1) ? (0xEDB88320u ^ (r >> 1)) : (r >> 1);
         stbiw__crc32_tab[i]=r;
      }
      stbiw__crc32_init=1;
   }
   for (int i=0;i<len;i++) c = stbiw__crc32_tab[(c ^ buffer[i]) & 0xff] ^ (c >> 8);
   return c ^ 0xffffffffu;
}

static unsigned int stbiw__adler32(const unsigned char *data, int len) {
   const unsigned int MOD_ADLER = 65521;
   unsigned int a=1, b=0;
   for (int i=0;i<len;i++) { a = (a + data[i]) % MOD_ADLER; b = (b + a) % MOD_ADLER; }
   return (b << 16) | a;
}

static void stbiw__write32be(FILE *f, unsigned int v) {
   unsigned char b[4];
   b[0]=(unsigned char)(v>>24);
   b[1]=(unsigned char)(v>>16);
   b[2]=(unsigned char)(v>>8);
   b[3]=(unsigned char)(v);
   fwrite(b,4,1,f);
}

static void stbiw__png_write_chunk(FILE *f, const char type[4], const unsigned char *data, int len) {
   stbiw__write32be(f, (unsigned int)len);
   fwrite(type, 4, 1, f);
   if (len) fwrite(data, (size_t)len, 1, f);
   unsigned int crc = 0;
   crc = stbiw__crc32(crc, (const unsigned char*)type, 4);
   if (len) crc = stbiw__crc32(crc, data, len);
   stbiw__write32be(f, crc);
}

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes) {
   if (w <= 0 || h <= 0) return 0;
   if (comp != 1 && comp != 2 && comp != 3 && comp != 4) return 0;
   FILE *f = NULL;
#if defined(_MSC_VER)
   if (fopen_s(&f, filename, "wb") != 0) return 0;
#else
   f = fopen(filename, "wb");
   if (!f) return 0;
#endif

   // PNG signature
   static const unsigned char sig[8] = { 137,80,78,71,13,10,26,10 };
   fwrite(sig, 8, 1, f);

   // IHDR
   unsigned char ihdr[13];
   ihdr[0]=(unsigned char)(w>>24); ihdr[1]=(unsigned char)(w>>16); ihdr[2]=(unsigned char)(w>>8); ihdr[3]=(unsigned char)w;
   ihdr[4]=(unsigned char)(h>>24); ihdr[5]=(unsigned char)(h>>16); ihdr[6]=(unsigned char)(h>>8); ihdr[7]=(unsigned char)h;
   ihdr[8]=8; // bit depth
   // color type
   ihdr[9] = (comp==1)?0 : (comp==2)?4 : (comp==3)?2 : 6;
   ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
   stbiw__png_write_chunk(f, "IHDR", ihdr, 13);

   // Build raw scanlines with filter byte 0
   const unsigned char *src = (const unsigned char*)data;
   int row_bytes = w * comp;
   std::vector<unsigned char> raw;
   raw.resize((size_t)h * (row_bytes + 1));
   for (int y=0; y<h; y++) {
      raw[(size_t)y * (row_bytes + 1)] = 0;
      memcpy(&raw[(size_t)y * (row_bytes + 1) + 1], src + (size_t)y * (size_t)stride_in_bytes, (size_t)row_bytes);
   }

   // zlib stream with stored (uncompressed) blocks
   std::vector<unsigned char> z;
   z.reserve(raw.size() + 6 + (raw.size()/65535+1)*5);
   // zlib header: CMF/FLG for deflate, 32K window
   z.push_back(0x78); z.push_back(0x01); // fastest / no compression

   size_t remaining = raw.size();
   size_t offset = 0;
   while (remaining > 0) {
      unsigned int block_len = (unsigned int)((remaining > 65535) ? 65535 : remaining);
      unsigned int bfinal = (remaining <= 65535) ? 1 : 0;
      z.push_back((unsigned char)(bfinal)); // BTYPE=00 stored
      z.push_back((unsigned char)(block_len & 0xff));
      z.push_back((unsigned char)((block_len >> 8) & 0xff));
      unsigned int nlen = 0xffffu - block_len;
      z.push_back((unsigned char)(nlen & 0xff));
      z.push_back((unsigned char)((nlen >> 8) & 0xff));
      z.insert(z.end(), raw.begin() + (ptrdiff_t)offset, raw.begin() + (ptrdiff_t)(offset + block_len));
      offset += block_len;
      remaining -= block_len;
   }

   unsigned int ad = stbiw__adler32(raw.data(), (int)raw.size());
   z.push_back((unsigned char)((ad >> 24) & 0xff));
   z.push_back((unsigned char)((ad >> 16) & 0xff));
   z.push_back((unsigned char)((ad >> 8) & 0xff));
   z.push_back((unsigned char)(ad & 0xff));

   stbiw__png_write_chunk(f, "IDAT", z.data(), (int)z.size());
   stbiw__png_write_chunk(f, "IEND", NULL, 0);
   fclose(f);
   return 1;
}

#endif // STB_IMAGE_WRITE_IMPLEMENTATION

