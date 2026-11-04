/*
 * Checks the EBCDIC character macros in port.h and the IBM-1047 table.
 *
 * Built with the library and run by the top-level `make check`.  In an
 * ASCII build there is nothing to check.  In an EBCDIC build on z/OS it
 * also compares charset.c with the system's own iconv IBM-1047 <->
 * ISO8859-1 conversion.  On a development host, `make test-ebcdic`
 * builds it with gcc -fexec-charset=IBM1047 (EBCDIC literals) and
 * LIBEDIT_EBCDIC_SIM; there it reports only through its exit status,
 * since glibc cannot read EBCDIC format strings.
 */
#include "config.h"

#if !LIBEDIT_EBCDIC
int main(void) {
    return 0; /* ASCII build: the macros are the identity */
}
#else
#ifdef __MVS__
#include <iconv.h>
#include <stdio.h>
#endif

#define SA(e) _Static_assert(e, #e)
/* constants usable in case labels and static initializers */
SA(EL_ESC == 0x27);
SA(EL_DEL == 0x07);
SA('\t' == 0x05 && '\b' == 0x16 && '\a' == 0x2f);
SA(EL_A2N_C(011) == '\t');
#ifndef LIBEDIT_EBCDIC_SIM /* gcc's IBM1047 has '\n' = 0x25, z/OS 0x15 */
SA('\n' == 0x15 && EL_A2N_C(012) == '\n');
#endif
SA(EL_CTRL_C('a') == 0x01);
SA(EL_CTRL_C('b') == 0x02);
SA(EL_CTRL_C('c') == 0x03);
SA(EL_CTRL_C('d') == 0x37);
SA(EL_CTRL_C('e') == 0x2d);
SA(EL_CTRL_C('f') == 0x2e);
SA(EL_CTRL_C('g') == 0x2f);
SA(EL_CTRL_C('h') == 0x16);
SA(EL_CTRL_C('i') == 0x05);
SA(EL_CTRL_C('j') == 0x15);
SA(EL_CTRL_C('k') == 0x0b);
SA(EL_CTRL_C('l') == 0x0c);
SA(EL_CTRL_C('m') == 0x0d);
SA(EL_CTRL_C('n') == 0x0e);
SA(EL_CTRL_C('o') == 0x0f);
SA(EL_CTRL_C('p') == 0x10);
SA(EL_CTRL_C('q') == 0x11);
SA(EL_CTRL_C('r') == 0x12);
SA(EL_CTRL_C('s') == 0x13);
SA(EL_CTRL_C('t') == 0x3c);
SA(EL_CTRL_C('u') == 0x3d);
SA(EL_CTRL_C('v') == 0x32);
SA(EL_CTRL_C('w') == 0x26);
SA(EL_CTRL_C('x') == 0x18);
SA(EL_CTRL_C('y') == 0x19);
SA(EL_CTRL_C('z') == 0x3f);
SA(EL_CTRL_C('A') == 0x01);
SA(EL_CTRL_C('B') == 0x02);
SA(EL_CTRL_C('C') == 0x03);
SA(EL_CTRL_C('D') == 0x37);
SA(EL_CTRL_C('E') == 0x2d);
SA(EL_CTRL_C('F') == 0x2e);
SA(EL_CTRL_C('G') == 0x2f);
SA(EL_CTRL_C('H') == 0x16);
SA(EL_CTRL_C('I') == 0x05);
SA(EL_CTRL_C('J') == 0x15);
SA(EL_CTRL_C('K') == 0x0b);
SA(EL_CTRL_C('L') == 0x0c);
SA(EL_CTRL_C('M') == 0x0d);
SA(EL_CTRL_C('N') == 0x0e);
SA(EL_CTRL_C('O') == 0x0f);
SA(EL_CTRL_C('P') == 0x10);
SA(EL_CTRL_C('Q') == 0x11);
SA(EL_CTRL_C('R') == 0x12);
SA(EL_CTRL_C('S') == 0x13);
SA(EL_CTRL_C('T') == 0x3c);
SA(EL_CTRL_C('U') == 0x3d);
SA(EL_CTRL_C('V') == 0x32);
SA(EL_CTRL_C('W') == 0x26);
SA(EL_CTRL_C('X') == 0x18);
SA(EL_CTRL_C('Y') == 0x19);
SA(EL_CTRL_C('Z') == 0x3f);
SA(EL_CTRL_C('[') == 0x27);
SA(EL_CTRL_C('\\') == 0x1c);
SA(EL_CTRL_C(']') == 0x1d);
SA(EL_CTRL_C('^') == 0x1e);
SA(EL_CTRL_C('_') == 0x1f);

int main(void) {
    int i, bad = 0;

    for (i = 0; i < 256; i++) {
        if (EL_N2A(EL_A2N(i)) != i) /* bijection */
            bad |= 1;
        if (i < 32 && EL_UNCTRL(EL_A2N(i)) != EL_A2N(i | 0100))
            bad |= 2;
        if (i < 128 && EL_META(EL_A2N(i), 0200) != EL_A2N(i | 0200))
            bad |= 4;
        if (i < 32 && EL_A2N_C(i) != EL_A2N(i)) /* const == table */
            bad |= 8;
    }
    if (EL_A2N_C(0177) != EL_A2N(0177))
        bad |= 8;
    /* letters: native literal, ASCII code, control char */
    if (EL_N2A('a') != 0x61 || EL_N2A('Z') != 0x5a || EL_N2A('[') != 0x5b)
        bad |= 16;
    if (EL_UNCTRL(EL_CTRL_C('x')) != ('X' & 0xff))
        bad |= 32;
    if (EL_META(1000, 0200) != 1000) /* wide chars pass through */
        bad |= 64;
    if (EL_WESC[0] != EL_ESC || EL_WESC[1] != 0)
        bad |= 128;
#ifdef __MVS__
    {
        /* charset.c must equal z/OS's IBM-1047 conversion */
        iconv_t cd = iconv_open("ISO8859-1", "IBM-1047");
        char in[256], out[256], *ip = in, *op = out;
        size_t il = 256, ol = 256;

        for (i = 0; i < 256; i++)
            in[i] = (char)i;
        if (cd == (iconv_t)-1 || iconv(cd, &ip, &il, &op, &ol) == (size_t)-1 ||
            ol != 0)
            bad |= 256;
        else
            for (i = 0; i < 256; i++)
                if ((unsigned char)out[i] != libedit_e2a[i]) {
                    fprintf(stderr,
                            "charset_test: e2a[%02X]: "
                            "table %02X, iconv %02X\n",
                            i, libedit_e2a[i], (unsigned char)out[i]);
                    bad |= 256;
                }
    }
    if (bad)
        fprintf(stderr, "charset_test: FAILED (%#x)\n", bad);
#endif
    return bad;
}
#endif /* LIBEDIT_EBCDIC */
