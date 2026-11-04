/*
 * Prompt expansion, bash-compatible:
 *
 *   \u user        \h host (short)  \H host (full)   \w cwd, ~ for HOME
 *   \W basename    \$ # if root     \s shell name    \v \V version
 *   \t HH:MM:SS    \T 12h HH:MM:SS  \@ 12h am/pm     \A HH:MM
 *   \d "Tue Nov 03"  \D{fmt} strftime              \j jobs
 *   \! history no. \# command no.   \n \r \a \e \\   \nnn octal
 *   \[ \]  bracket non-printing text (colors) so line width is right
 *
 * Backslash escapes are decoded first, then the result gets parameter
 * expansion, command substitution and arithmetic, as with bash's
 * promptvars.  Text produced by \w, \W, \u, \h and \H is quoted before
 * that second pass, so a directory named "$(rm -rf ~)" is shown, not
 * run (bash quotes these too since 4.4).
 */
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

#define ZASH_VERSION "0.1"
#define ZASH_RELEASE "0.1.0"

/* ESC in the build's charset: 0x1B in ASCII, 0x27 in EBCDIC. */
#define CH_ESC ('A' == 0x41 ? 0x1b : 0x27)

static char *user_name, *host_full;

static void
init_ids(void)
{
	struct passwd *pw;
	char buf[256];

	if (user_name)
		return;
	pw = getpwuid(geteuid());
	user_name = xstrdup(pw && pw->pw_name ? pw->pw_name : "?");
	if (gethostname(buf, sizeof(buf)) != 0)
		strcpy(buf, "localhost");
	buf[sizeof(buf) - 1] = '\0';
	host_full = xstrdup(buf);
}

/* Append s so that the expansion pass leaves it literal. */
static void
put_quoted(struct sbuf *b, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (strchr("$`\\\"'", s[i]))
			sb_putc(b, '\\');
		sb_putc(b, s[i]);
	}
}

static void
put_cwd(struct sbuf *b, int base_only)
{
	const char *pwd = var_get("PWD"), *home = var_get("HOME"), *p;
	char buf[4096];
	size_t hl;

	if (pwd == NULL || pwd[0] != '/')
		pwd = getcwd(buf, sizeof(buf)) ? buf : "?";
	hl = home ? strlen(home) : 0;
	if (base_only) {
		if (hl > 1 && strcmp(pwd, home) == 0) {
			sb_putc(b, '~');
			return;
		}
		p = strrchr(pwd, '/');
		p = p && p[1] ? p + 1 : pwd;
		put_quoted(b, p, strlen(p));
		return;
	}
	if (hl > 1 && strncmp(pwd, home, hl) == 0 &&
	    (pwd[hl] == '/' || pwd[hl] == '\0')) {
		sb_putc(b, '~');
		pwd += hl;
	}
	put_quoted(b, pwd, strlen(pwd));
}

static void
put_time(struct sbuf *b, const char *fmt)
{
	char buf[128];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	if (tm && strftime(buf, sizeof(buf), fmt, tm) > 0)
		sb_puts(b, buf);
}

static void
decode(const char *ps, struct sbuf *b)
{
	const char *p;
	char num[32];

	for (p = ps; *p; p++) {
		if (*p != '\\' || p[1] == '\0') {
			sb_putc(b, *p);
			continue;
		}
		switch (*++p) {
		case 'u': put_quoted(b, user_name, strlen(user_name)); break;
		case 'H': put_quoted(b, host_full, strlen(host_full)); break;
		case 'h':
			put_quoted(b, host_full, strcspn(host_full, "."));
			break;
		case 'w': put_cwd(b, 0); break;
		case 'W': put_cwd(b, 1); break;
		case '$': sb_putc(b, geteuid() == 0 ? '#' : '$'); break;
		case 's': sb_puts(b, "zash"); break;
		case 'v': sb_puts(b, ZASH_VERSION); break;
		case 'V': sb_puts(b, ZASH_RELEASE); break;
		case 't': put_time(b, "%H:%M:%S"); break;
		case 'T': put_time(b, "%I:%M:%S"); break;
		case '@': put_time(b, "%I:%M %p"); break;
		case 'A': put_time(b, "%H:%M"); break;
		case 'd': put_time(b, "%a %b %d"); break;
		case 'D':
			if (p[1] == '{') {
				const char *e = strchr(p + 2, '}');

				if (e) {
					char fmt[64];
					size_t n = (size_t)(e - p - 2);

					if (n == 0)
						put_time(b, "%X");
					else if (n < sizeof(fmt)) {
						memcpy(fmt, p + 2, n);
						fmt[n] = '\0';
						put_time(b, fmt);
					}
					p = e;
					break;
				}
			}
			sb_puts(b, "\\D");
			break;
		case 'j':
			snprintf(num, sizeof(num), "%d", job_count());
			sb_puts(b, num);
			break;
		case '!':
			snprintf(num, sizeof(num), "%d", zhist_next_number());
			sb_puts(b, num);
			break;
		case '#':
			snprintf(num, sizeof(num), "%d", sh.cmdno + 1);
			sb_puts(b, num);
			break;
		case 'n': sb_putc(b, '\n'); break;
		case 'r': sb_putc(b, '\r'); break;
		case 'a': sb_putc(b, '\a'); break;
		case 'e': sb_putc(b, CH_ESC); break;
		case '[':
		case ']': sb_putc(b, PROMPT_IGNORE); break;
		case '\\': sb_puts(b, "\\\\"); break;
		case '0': case '1': case '2': case '3':
		case '4': case '5': case '6': case '7': {
			int v = 0, k;

			for (k = 0; k < 3 && *p >= '0' && *p <= '7'; k++, p++)
				v = v * 8 + (*p - '0');
			p--;
			/* \033 means ESC in prompts copied from ASCII systems */
			sb_putc(b, v == 033 ? CH_ESC : v);
			break;
		}
		default:
			sb_putc(b, '\\');
			sb_putc(b, *p);
		}
	}
}

/*
 * Returns the expanded prompt in a static buffer.  Errors during the
 * expansion pass (e.g. ${x?}) fall back to the decoded text instead of
 * aborting the read loop.
 */
const char *
prompt_expand(const char *ps)
{
	static struct sbuf out;
	struct sbuf dec = {0};
	jmp_buf jb, *prev = sh.errjmp;
	int sav_status = sh.status;
	const char *r;

	init_ids();
	decode(ps, &dec);
	sb_cstr(&dec);
	sh.errjmp = &jb;
	if (setjmp(jb) == 0)
		r = expand_str(dec.s, 0);
	else
		r = dec.s;      /* show it unexpanded rather than loop */
	sh.errjmp = prev;
	sh.status = sav_status; /* $(...) in PS1 must not clobber $? */
	out.len = 0;
	sb_puts(&out, r);
	sb_free(&dec);
	return sb_cstr(&out);
}
