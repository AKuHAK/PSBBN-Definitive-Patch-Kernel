/* genpat SIZE SEED FILE  - write a pattern file;  genpat -c SIZE SEED FILE - check one */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char pat(long long off, int seed) { return (unsigned char)((off * 7 + seed + (off >> 9)) & 0xff); }
int main(int argc, char **argv)
{
	int check = argc > 1 && !strcmp(argv[1], "-c");
	long long size, i; int seed, c; FILE *f;
	if (argc != 4 + check) return 2;
	size = atoll(argv[1 + check]); seed = atoi(argv[2 + check]);
	f = fopen(argv[3 + check], check ? "rb" : "wb");
	if (!f) return 3;
	for (i = 0; i < size; i++) {
		if (!check) { putc(pat(i, seed), f); continue; }
		if ((c = getc(f)) != pat(i, seed)) { printf("%s: mismatch at %lld\n", argv[4], i); return 1; }
	}
	if (check && getc(f) != EOF) { printf("%s: longer than %lld\n", argv[4], size); return 1; }
	return fclose(f) ? 4 : 0;
}
