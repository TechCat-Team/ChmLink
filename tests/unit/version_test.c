#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "chmlink/version.h"

/* Keeps driver/chmlink.inf DriverVer and version.h in lockstep. */
int main(int argc, char **argv)
{
    char line[512], expected[64];
    FILE *inf;
    int found = 0;
    snprintf(expected, sizeof(expected), "%u.%u.%u.%u", CHMLINK_VERSION_MAJOR,
             CHMLINK_VERSION_MINOR, CHMLINK_VERSION_PATCH, CHMLINK_VERSION_BUILD);
    if (strcmp(expected, CHMLINK_VERSION_STRING) != 0) {
        fprintf(stderr, "CHMLINK_VERSION_STRING %s != numeric %s\n",
                CHMLINK_VERSION_STRING, expected);
        return 1;
    }
    if (argc != 2 || (inf = fopen(argv[1], "r")) == NULL) {
        fprintf(stderr, "usage: version_test <chmlink.inf>\n");
        return 1;
    }
    while (fgets(line, sizeof(line), inf) != NULL) {
        char *p = line, *comma, *end;
        while (isspace((unsigned char)*p)) ++p;
        if (strncmp(p, "DriverVer", 9) != 0) continue;
        comma = strchr(p, ',');
        if (comma == NULL) break;
        for (p = comma + 1; isspace((unsigned char)*p); ++p) {}
        for (end = p; *end && !isspace((unsigned char)*end) && *end != ';'; ++end) {}
        *end = '\0';
        if (found++ || strcmp(p, CHMLINK_VERSION_STRING) != 0) {
            fprintf(stderr, "INF DriverVer %s does not match %s\n", p, CHMLINK_VERSION_STRING);
            fclose(inf);
            return 1;
        }
    }
    fclose(inf);
    if (found != 1) { fprintf(stderr, "INF has no DriverVer\n"); return 1; }
    return 0;
}
