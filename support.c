#include <ctype.h>

#include "support.h"

/* Skip whitespace characters in string starting from offset i. Returns offset
 * j >= i as the next non-whitespace character offset, or -1 if non-whitespace
 * are not found.
 */
int skipws(const char *s, int i)
{
	while (isspace(s[i]))
		i++;

	if (s[i] == 0)
		return -1;

	return i;
}

/* Skip non-whitespace characters in string starting from offset i. Returns
 * offset j >= i as the next whitespace character offset, or -1 if no
 * whitespace if found.
 */
int skipnws(const char *s, int i)
{
	while (!isspace(s[i]) && s[i] != 0)
		i++;

	if (s[i] == 0)
		return -1;

	return i;
}

size_t xfwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret;
	size_t written = 0;
	char *writeptr = (char *) ptr;

	while (written < nmemb) {
		ret = fwrite(writeptr, size, nmemb - written, stream);
		if (ret == 0)
			break;
		written += ret;
		writeptr += size * ret;
	}

	return written;
}
