/*
 * Facts the EBCDIC libedit design depends on, measured rather than
 * guessed.  Compiled once per mode by probe.sh.  probe_a.c is always
 * compiled in ASCII mode and linked in, to test mixing ASCII and
 * EBCDIC compilation units in one program.
 */
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

int probe_ascii_unit(unsigned *wa, int *iswa, char *clear, size_t n);

int main(void) {
    char a = 'a', mb[8];
    wchar_t wl = L'a', w = 0;
    mbstate_t st;
    unsigned wa = 0;
    int iswa = -1, i;
    char clear[16];
    size_t r;

    setlocale(LC_ALL, "");
    printf("mode            %s\n", 'A' == 0x41 ? "ASCII" : "EBCDIC");
    printf("codeset         %s\n", nl_langinfo(CODESET));
    printf("sizeof(wchar_t) %u\n", (unsigned)sizeof(wchar_t));
    printf("'a'             0x%02X\n", (unsigned)(unsigned char)a);
    printf("L'a'            0x%04X\n", (unsigned)wl);
    printf("'\\n' L'\\n'      0x%02X 0x%04X\n", (unsigned)(unsigned char)'\n',
           (unsigned)L'\n');

    memset(&st, 0, sizeof(st));
    r = mbrtowc(&w, &a, 1, &st);
    printf("mbrtowc('a')    0x%04X (rc %d)\n", (unsigned)w, (int)r);
    printf("  == L'a'?      %s\n", w == wl ? "yes" : "NO");
    printf("iswalpha(L'a')  %d\n", iswalpha((wint_t)wl) != 0);
    printf("iswalpha(0x61)  %d   iswalpha(0x81) %d\n", iswalpha(0x61) != 0,
           iswalpha(0x81) != 0);
    memset(&st, 0, sizeof(st));
    r = wcrtomb(mb, wl, &st);
    printf("wcrtomb(L'a')   0x%02X (rc %d)\n", (unsigned)(unsigned char)mb[0],
           (int)r);

    clear[0] = '\0';
    if (probe_ascii_unit(&wa, &iswa, clear, sizeof(clear)) == 0) {
        printf("ASCII unit      linked and ran\n");
        printf("  its L'a'      0x%04X  iswalpha %d\n", wa, iswa);
        printf("  curses clear  ");
        for (i = 0; clear[i] && i < 6; i++)
            printf("%02X ", (unsigned)(unsigned char)clear[i]);
        printf("%s\n", clear[0] == 0x1b   ? "(ASCII ESC)"
                       : clear[0] == 0x27 ? "(EBCDIC ESC)"
                                          : "(none)");
    }
    return 0;
}
