// Re-emitted from the original MSVC __asm block -- do not hand-edit; regenerate with
//   python3 tools/asmport/emit.py <file.cpp> SampleFogLight
static void SampleFogLight_asm()
{
    __asm__ __volatile__ (
        "SampleFogLight_SampleFogLight:\n\t"
        "movq mm1, mm7\n\t"   // dup current lit position to obtain fractional bits
        "movq mm0, mm7\n\t"   // dup current lit position to obtain integer bits
        "psrld mm0, 16\n\t"   // Shift right logical the 2 dwords: get integers into low words of the 2 dwords.
        "pand mm0, [LightMip + 0]\n\t"   // Mask out: 2 dwords:  (VSize-1):(USize-1)
        "psrlw mm1, 1\n\t"   // make 15-bit fractional coordinates (shift 0 into bit 15)
        "packssdw mm0, mm0\n\t"   // Pack doublewords to words -> low 32 bits are now (Vcoord:Ucoord)
        "pmaddwd mm0, [LightMip + 16]\n\t"   // Mul by (SrcMip.USize<<16) : 1 (2 different multiplications) and sum
        "paddd mm7, [MMXLitDelta]\n\t"   // Lit + LitDelta
        "movd eax, mm0\n\t"   // Our index into the lightmap.
        "movq mm2, mm1\n\t"   // 15-bit fractional lightmap index
        "movq [MMXLit], mm7\n\t"   // save...
        "punpcklwd mm1, mm1\n\t"   // 00:00:xx:16  to  xx:xx:16:16
        "mov edi, [LightMip + 24]\n\t"
        "punpckldq mm1, mm1\n\t"   // 00:00:16:16  to  16:16:16:16 - quad expansion of fractional U coordinate
        "mov edx, [LightMip + 28]\n\t"
        "punpckhwd mm2, mm2\n\t"   // xx:16:00:00  to  xx:xx:16:16
        "punpckldq mm2, mm2\n\t"   // xx:xx:16:16 to 16:16:16:16 -  fractional V coordinate
        "lea ebx, [edx + eax*4]\n\t"   // Full pointer into lightmap.
        "pxor mm4, mm4\n\t"   // zeroes needed when unpacking bytes to words...
        "pxor mm5, mm5\n\t"
        "punpcklbw mm4, [ebx + 0]\n\t"   // A - low 4 bytes of source dword into 4 words
        "pxor mm6, mm6\n\t"
        "punpcklbw mm5, [ebx + 4]\n\t"   // B
        "pxor mm7, mm7\n\t"
        "punpcklbw mm6, [ebx + edi*4]\n\t"   // C
        "psubsw mm5, mm4\n\t"   // B - A (signed)
        "punpcklbw mm7, [ebx + edi*4 + 4]\n\t"   // D
        "pmulhw mm5, mm1\n\t"
        "psubsw mm7, mm6\n\t"   // D - C (signed)
        "pmulhw mm7, mm1\n\t"
        "psllw mm5, 1\n\t"   // shift left: to get into proper alignment for adding mm4
        "psllw mm7, 1\n\t"   // shift left: to get into proper alignment for adding mm6
        "paddw mm5, mm4\n\t"   // I = X(B-A) + A; -> COULD be a signed number... ?!
        "paddw mm7, mm6\n\t"   // J = X(D-C) + C; -> COULD be a signed number... ?!
        "psubsw mm7, mm5\n\t"   // J - I
        "pmulhw mm7, mm2\n\t"   // Y (J-I) // U pipe only
        "psrlw mm5, 1\n\t"   // pshift right: proper alignment for adding to mm7...
        "paddw mm7, mm5\n\t"   // Y (J-I) + I     //#debug latency....
        "mov edx, [LightMip + 32]\n\t"
        "lea ebx, [edx + eax*4]\n\t"   // Full pointer into lightmap.
        "pxor mm4, mm4\n\t"   // zeroes needed when unpacking bytes to words...
        "pxor mm5, mm5\n\t"
        "punpcklbw mm4, [ebx + 0]\n\t"   // A - low 4 bytes of source dword into 4 words
        "pxor mm6, mm6\n\t"
        "punpcklbw mm5, [ebx + 4]\n\t"   // B
        "pxor mm3, mm3\n\t"
        "punpcklbw mm6, [ebx + edi*4]\n\t"   // C
        "punpcklbw mm3, [ebx + edi*4 + 4]\n\t"   // D
        "psubsw mm5, mm4\n\t"   // B - A (signed)
        "pmulhw mm5, mm1\n\t"
        "psubsw mm3, mm6\n\t"   // D - C (signed)
        "pmulhw mm3, mm1\n\t"
        "psllw mm5, 1\n\t"   // shift left: to get into proper alignment for adding mm4
        "psllw mm3, 1\n\t"   // shift left: to get into proper alignment for adding mm6
        "paddw mm5, mm4\n\t"   // I = X(B-A) + A; -> COULD be a signed number... ?!
        "paddw mm3, mm6\n\t"   // J = X(D-C) + C; -> COULD be a signed number... ?!
        "psubsw mm3, mm5\n\t"   // J - I
        "pmulhw mm3, mm2\n\t"   // Y (J-I) // U pipe only
        "psrlw mm5, 1\n\t"   // pshift right: proper alignment for adding to mm3...
        "pmulhw mm7, [MMXFlashCompress]\n\t"   // FlashCompress: 15-bit doublebright 15 bits, but "1" actually 14...
        "paddw mm3, mm5\n\t"   // Y (J-I) + I     //#debug latency....
        "movq mm4, [MMX14Bits]\n\t"   // #else
        "psubsw mm4, mm3\n\t"   // 1-fog = fogcompress
        "pmulhw mm3, [MMXFlashCompress]\n\t"   // #endif  Fogoffset * FlashCompress   = 14*14 = 12 bits
        "pmulhw mm7, mm4\n\t"   // light * flashcompress * (1-fogmap)... 14*14*14 = 10 bits left.
        "paddw mm3, [MMXFlashOffset13]\n\t"   // + 13-bit FlashOffset
        "psllw mm7, 4\n\t"   // make 14 bits again.(actually full 15 bit)
        "movq [ebp + 8], mm3\n\t"   // Store Fog   - 13-bit scale.
        "movq [ebp + 0], mm7\n\t"   // Store Light - 14-bit scale.
        "add ebp, 16\n\t"
        ::: "eax","ebx","edi","edx","memory" );
}

