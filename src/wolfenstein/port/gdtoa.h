#ifndef ECWOLF_OPENFPGA_GDTOA_H
#define ECWOLF_OPENFPGA_GDTOA_H

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline char *of_gdtoa_strdup_digits(const char *text, char **end)
{
    size_t len = strlen(text);
    char *out = (char *)malloc(len + 1);
    char *p = out;
    if (!out)
        return NULL;

    for (const char *s = text; *s; ++s)
    {
        if (*s >= '0' && *s <= '9')
            *p++ = *s;
    }
    if (p == out)
        *p++ = '0';
    *p = '\0';
    if (end)
        *end = p;
    return out;
}

static inline char *dtoa(double value, int mode, int ndigits,
                         int *decpt, int *sign, char **rve)
{
    char buf[96];
    char *eptr;
    int exp = 0;

    if (sign)
        *sign = value < 0.0;
    if (value < 0.0)
        value = -value;

    if (isnan(value))
    {
        if (decpt)
            *decpt = INT_MAX;
        char *out = (char *)malloc(4);
        if (out)
            memcpy(out, "NaN", 4);
        if (rve)
            *rve = out ? out + 3 : NULL;
        return out;
    }
    if (isinf(value))
    {
        if (decpt)
            *decpt = INT_MAX;
        char *out = (char *)malloc(4);
        if (out)
            memcpy(out, "Inf", 4);
        if (rve)
            *rve = out ? out + 3 : NULL;
        return out;
    }

    if (ndigits < 0)
        ndigits = 6;

    if (mode == 2)
    {
        int precision = ndigits > 0 ? ndigits - 1 : 0;
        snprintf(buf, sizeof(buf), "%.*e", precision, value);
        eptr = strchr(buf, 'e');
        if (!eptr)
            eptr = strchr(buf, 'E');
        if (eptr)
        {
            exp = atoi(eptr + 1);
            *eptr = '\0';
        }
        if (decpt)
            *decpt = exp + 1;
        return of_gdtoa_strdup_digits(buf, rve);
    }

    snprintf(buf, sizeof(buf), "%.*f", ndigits, value);
    eptr = strchr(buf, '.');
    if (decpt)
        *decpt = eptr ? (int)(eptr - buf) : (int)strlen(buf);
    return of_gdtoa_strdup_digits(buf, rve);
}

static inline void freedtoa(char *s)
{
    free(s);
}

#endif
