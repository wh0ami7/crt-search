/* crt-search.c – fetch common-name identities from crt.sh
 *   • output to stdout **and** <domain>_identities.txt
 *   • C23-compatible, works with gcc on Arch Linux
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#include <regex.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define MAX_DOMAIN_LEN   256
#define MAX_IDENTITY_LEN 1024
#define MAX_IDENTITIES   1000000
#define LOG_FILE         "script_errors.log"

/* --------------------------------------------------------------- */
/* simple fatal-error helper – writes to stderr + LOG_FILE */
static void fatal_error(const char *msg)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(stderr, "[%s] ERROR: %s\n", ts, msg);

    FILE *log = fopen(LOG_FILE, "a");
    if (log) {
        fprintf(log, "[%s] ERROR: %s\n", ts, msg);
        fclose(log);
    }
    exit(EXIT_FAILURE);
}

/* --------------------------------------------------------------- */
/* domain validation – length, allowed chars, no dangerous chars */
static int validate_domain(const char *domain)
{
    size_t len = strlen(domain);
    if (len == 0 || len > 255) {
        fatal_error("Domain length must be 1-255 characters");
    }

    if (strpbrk(domain, "'\";`") != NULL) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "Domain '%s' contains invalid characters (quotes, semicolon, backtick)",
                 domain);
        fatal_error(buf);
    }

    regex_t re;
    if (regcomp(&re, "^[a-zA-Z0-9.-]*$", REG_EXTENDED) != 0) {
        fatal_error("Regex compilation failed");
    }
    int rc = regexec(&re, domain, 0, NULL, 0);
    regfree(&re);
    if (rc != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "Domain '%s' must contain only alphanumeric, dot or hyphen",
                 domain);
        fatal_error(buf);
    }
    return 1;
}

/* --------------------------------------------------------------- */
/* test that we can reach the DB */
static int check_connectivity(void)
{
    const char *ci = "host=crt.sh port=5432 user=guest dbname=certwatch "
                     "sslmode=require connect_timeout=10";
    PGconn *conn = PQconnectdb(ci);
    if (PQstatus(conn) != CONNECTION_OK) {
        fatal_error(PQerrorMessage(conn));
    }
    PQfinish(conn);
    return 1;
}

/* --------------------------------------------------------------- */
/* bubble-sort helper – works on any null-terminated array of strings */
static void bubble_sort(char **arr, int n)
{
    for (int i = 0; i < n - 1; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (strcmp(arr[i], arr[j]) > 0) {
                char *tmp = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
            }
        }
    }
}

/* --------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <domain>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *domain = argv[1];
    char output_file[MAX_DOMAIN_LEN + 20] = {0};
    snprintf(output_file, sizeof(output_file), "%s_identities.txt", domain);

    validate_domain(domain);
    check_connectivity();

    /* make sure the directory for the output file is writable */
    {
        char *slash = strrchr(output_file, '/');
        if (slash) {
            *slash = '\0';
            struct stat st = {0};
            if (stat(output_file, &st) != 0 || !S_ISDIR(st.st_mode) ||
                access(output_file, W_OK) != 0) {
                fatal_error("Output directory is not writable");
            }
            *slash = '/';
        }
    }

    /* ---------- connect & run parameterised query ---------- */
    const char *conninfo = "host=crt.sh port=5432 user=guest dbname=certwatch "
                           "sslmode=require connect_timeout=10";
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        fatal_error(PQerrorMessage(conn));
    }

    const char *sql =
        "SELECT cai.NAME_VALUE "
        "FROM certificate_and_identities cai "
        "WHERE plainto_tsquery('certwatch', $1) @@ identities(cai.CERTIFICATE) "
        "  AND cai.NAME_VALUE ILIKE '%' || $1 || '%' "
        "  AND cai.NAME_TYPE = '2.5.4.3' "
        "LIMIT 1000000;";

    const char *paramValues[1] = { domain };
    PGresult *res = PQexecParams(conn, sql,
                                 1, NULL, paramValues, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fatal_error(PQerrorMessage(conn));
    }

    /* ---------- collect raw rows ---------- */
    int rows = PQntuples(res);
    char **raw = (char **)calloc(rows, sizeof(char *));
    int raw_cnt = 0;
    for (int i = 0; i < rows && raw_cnt < MAX_IDENTITIES; ++i) {
        const char *val = PQgetvalue(res, i, 0);
        if (val && *val) {
            raw[raw_cnt++] = strdup(val);
        }
    }
    PQclear(res);
    PQfinish(conn);

    /* ---------- uniqueness (simple linear set) ---------- */
    char **unique = (char **)calloc(raw_cnt, sizeof(char *));
    int uniq_cnt = 0;
    for (int i = 0; i < raw_cnt; ++i) {
        int dup = 0;
        for (int j = 0; j < uniq_cnt; ++j) {
            if (strcmp(raw[i], unique[j]) == 0) { dup = 1; break; }
        }
        if (!dup) unique[uniq_cnt++] = raw[i];
        else free(raw[i]);               /* discard duplicate */
    }
    free(raw);

    /* ---------- partition into wildcards / non-wildcards ---------- */
    char **wild = (char **)calloc(uniq_cnt, sizeof(char *));
    char **norm = (char **)calloc(uniq_cnt, sizeof(char *));
    int wild_cnt = 0, norm_cnt = 0;

    for (int i = 0; i < uniq_cnt; ++i) {
        if (strncmp(unique[i], "*.", 2) == 0)
            wild[wild_cnt++] = unique[i];
        else
            norm[norm_cnt++] = unique[i];
    }

    /* ---------- sort each partition ---------- */
    bubble_sort(wild, wild_cnt);
    bubble_sort(norm, norm_cnt);

    /* ---------- output to stdout + file ---------- */
    FILE *out = fopen(output_file, "w");
    if (!out) fatal_error("Cannot create output file");

    for (int i = 0; i < wild_cnt; ++i) {
        printf("%s\n", wild[i]);
        fprintf(out, "%s\n", wild[i]);
    }
    for (int i = 0; i < norm_cnt; ++i) {
        printf("%s\n", norm[i]);
        fprintf(out, "%s\n", norm[i]);
    }
    fclose(out);

    printf("Output saved to %s\n", output_file);

    /* ---------- cleanup ---------- */
    free(wild);
    free(norm);
    free(unique);
    return 0;
}
