/* Always compiled in ASCII mode (see probe.sh). */
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#ifdef PROBE_CURSES
#include <curses.h>
#include <term.h>
#endif

int
probe_ascii_unit(unsigned *wa, int *iswa, char *clear, size_t n)
{
	*wa = (unsigned)L'a';
	*iswa = iswalpha((wint_t)L'a') != 0;
	clear[0] = '\0';
#ifdef PROBE_CURSES
	{
		static char buf[2048], area[256];
		char *ap = area, *s;

		if (tgetent(buf, "xterm") == 1 &&
		    (s = tgetstr("cl", &ap)) != NULL) {
			strncpy(clear, s, n - 1);
			clear[n - 1] = '\0';
		}
	}
#else
	(void)n;
#endif
	return 0;
}
