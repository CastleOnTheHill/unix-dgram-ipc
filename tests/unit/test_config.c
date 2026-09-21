/*
 * Unit tests for the static module table parser.
 * Configuration mistakes (typos, duplicate modules, over-long paths) must be
 * loud, at load time, not at 03:00 when a datagram silently goes nowhere.
 */
#include <stdlib.h>
#include <string.h>

#include "ipc/ipc.h"
#include "ipc_config.h"
#include "utest.h"

static int parse_ok(const char *text, ipc_config_t **out)
{
    return ipc_config_parse(text, out);
}

static int parse_line_ok(const char *line)
{
    char               buf[512];
    ipc_config_entry_t e;
    int                rc;

    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    rc                   = ipc_config_parse_line(buf, &e, 1);
    return rc;
}

/* ------------------------------------------------------------------ */

UT_TEST(config, parses_a_normal_table)
{
    const char *text =
        "# namespace module uid socket_path\n"
        "core A1 1001 /run/example-ipc/A/A1.sock\n"
        "\n"
        "core A2 1001 /run/example-ipc/A/A2.sock   # trailing comment\n"
        "core B1 1002 /run/example-ipc/B/B1.sock\n"
        "extra Z9 1003 /run/other/Z9.sock\n";
    ipc_config_t             *cfg = NULL;
    const ipc_config_entry_t *e;

    UT_EQ_INT(parse_ok(text, &cfg), IPC_OK);
    UT_CHECK(cfg != NULL);
    UT_EQ_INT(ipc_config_count(cfg), 4);

    e = ipc_config_lookup(cfg, "core", "A1");
    UT_CHECK(e != NULL);
    UT_EQ_INT(e->uid, 1001);
    UT_EQ_STR(e->path, "/run/example-ipc/A/A1.sock");

    e = ipc_config_lookup(cfg, "core", "B1");
    UT_CHECK(e != NULL);
    UT_EQ_INT(e->uid, 1002);

    UT_CHECK(ipc_config_lookup(cfg, "core", "nope") == NULL);
    UT_CHECK(ipc_config_lookup(cfg, "nosuch", "A1") == NULL);
    UT_CHECK(ipc_config_lookup(cfg, NULL, "A1") == NULL);

    e = ipc_config_lookup_path(cfg, "/run/other/Z9.sock");
    UT_CHECK(e != NULL);
    UT_EQ_STR(e->ns, "extra");
    UT_EQ_STR(e->module, "Z9");
    UT_CHECK(ipc_config_lookup_path(cfg, "/nope") == NULL);

    UT_CHECK(ipc_config_at(cfg, 0) != NULL);
    UT_CHECK(ipc_config_at(cfg, 3) != NULL);
    UT_CHECK(ipc_config_at(cfg, 4) == NULL);
    UT_CHECK(ipc_config_at(cfg, -1) == NULL);

    ipc_config_free(cfg);
}

UT_TEST(config, same_module_in_two_namespaces_is_legal_but_ambiguous_later)
{
    const char  *text = "core A1 1001 /run/core/A1.sock\n"
                        "extra A1 1001 /run/extra/A1.sock\n";
    ipc_config_t *cfg = NULL;

    /* The table itself accepts it -- conflicts are a *global* property, and
     * ipc_register() refuses to guess (see IPC_ERR_CONFIG there). */
    UT_EQ_INT(parse_ok(text, &cfg), IPC_OK);
    UT_EQ_INT(ipc_config_count(cfg), 2);
    UT_EQ_STR(ipc_config_lookup(cfg, "core", "A1")->path, "/run/core/A1.sock");
    UT_EQ_STR(ipc_config_lookup(cfg, "extra", "A1")->path,
              "/run/extra/A1.sock");
    ipc_config_free(cfg);
}

UT_TEST(config, rejects_duplicate_module_in_same_namespace)
{
    ipc_config_t *cfg = NULL;
    UT_EQ_INT(parse_ok("core A1 1001 /run/A1.sock\n"
                       "core A1 1002 /run/B/A1.sock\n",
                       &cfg),
              IPC_ERR_CONFIG);
    UT_CHECK(cfg == NULL);
}

UT_TEST(config, rejects_duplicate_socket_path)
{
    ipc_config_t *cfg = NULL;
    /* Two modules pointing at one path would silently steal each other's
     * messages.  This must never load. */
    UT_EQ_INT(parse_ok("core A1 1001 /run/one.sock\n"
                       "core A2 1001 /run/one.sock\n",
                       &cfg),
              IPC_ERR_CONFIG);
    UT_CHECK(cfg == NULL);
}

UT_TEST(config, field_count_is_enforced)
{
    UT_EQ_INT(parse_line_ok("core A1 1001"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1 1001 /run/a.sock extra"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("     "), IPC_ERR_CONFIG);
}

UT_TEST(config, uid_must_be_a_plain_decimal)
{
    UT_EQ_INT(parse_line_ok("core A1 -1 /run/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1 +1001 /run/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1 10a /run/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1  /run/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1 abc /run/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1 4294967295 /run/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1 0 /run/a.sock"), IPC_OK);
    UT_EQ_INT(parse_line_ok("core A1 1001 /run/a.sock"), IPC_OK);
}

UT_TEST(config, socket_path_rules)
{
    char path[200];
    char line[256];
    int  i;

    UT_EQ_INT(parse_line_ok("core A1 1001 relative/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A1 1001 /"), IPC_ERR_CONFIG);

    /* exactly IPC_SUN_PATH_MAX - 1 characters is the last legal length */
    path[0] = '/';
    for (i = 1; i < IPC_SUN_PATH_MAX - 1; i++) {
        path[i] = 'a';
    }
    path[IPC_SUN_PATH_MAX - 1] = '\0';
    UT_EQ_INT(strlen(path), IPC_SUN_PATH_MAX - 1);
    snprintf(line, sizeof(line), "core A1 1001 %s", path);
    UT_EQ_INT(parse_line_ok(line), IPC_OK);

    path[IPC_SUN_PATH_MAX - 1] = 'a';
    path[IPC_SUN_PATH_MAX]     = '\0';
    UT_EQ_INT(strlen(path), IPC_SUN_PATH_MAX);
    snprintf(line, sizeof(line), "core A1 1001 %s", path);
    UT_EQ_INT(parse_line_ok(line), IPC_ERR_CONFIG);
}

UT_TEST(config, name_rules)
{
    char ns[64];
    char mod[64];

    memset(ns, 'n', sizeof(ns));
    ns[IPC_NS_MAX - 1] = '\0'; /* 15 chars: longest legal namespace */
    {
        char line[256];
        snprintf(line, sizeof(line), "%s A1 1001 /run/a.sock", ns);
        UT_EQ_INT(parse_line_ok(line), IPC_OK);
        ns[IPC_NS_MAX - 1] = 'n';
        ns[IPC_NS_MAX]     = '\0'; /* 16 chars: too long */
        snprintf(line, sizeof(line), "%s A1 1001 /run/a.sock", ns);
        UT_EQ_INT(parse_line_ok(line), IPC_ERR_CONFIG);
    }

    memset(mod, 'm', sizeof(mod));
    mod[IPC_NAME_MAX - 1] = '\0'; /* 31 chars: longest legal module id */
    {
        char line[256];
        snprintf(line, sizeof(line), "core %s 1001 /run/a.sock", mod);
        UT_EQ_INT(parse_line_ok(line), IPC_OK);
        mod[IPC_NAME_MAX - 1] = 'm';
        mod[IPC_NAME_MAX]     = '\0'; /* 32 chars: too long */
        snprintf(line, sizeof(line), "core %s 1001 /run/a.sock", mod);
        UT_EQ_INT(parse_line_ok(line), IPC_ERR_CONFIG);
    }

    UT_EQ_INT(parse_line_ok("core a/b 1001 /run/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("co re A1 1001 /run/a.sock"), IPC_ERR_CONFIG);
    UT_EQ_INT(parse_line_ok("core A-1_x.y 1001 /run/a.sock"), IPC_OK);
}

UT_TEST(config, bad_arguments_are_rejected)
{
    ipc_config_t *cfg = NULL;

    UT_EQ_INT(ipc_config_parse(NULL, &cfg), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_config_parse("core A1 1001 /run/a.sock", NULL), IPC_ERR_INVAL);
    ipc_config_free(NULL); /* must not crash */
    UT_EQ_INT(ipc_config_count(NULL), 0);
    UT_CHECK(ipc_config_at(NULL, 0) == NULL);
    UT_CHECK(ipc_config_lookup(NULL, "core", "A1") == NULL);
    UT_CHECK(ipc_config_lookup_path(NULL, "/x") == NULL);
}

UT_TEST(config, empty_and_comment_only_text_is_an_empty_table)
{
    ipc_config_t *cfg = NULL;

    UT_EQ_INT(parse_ok("", &cfg), IPC_OK);
    UT_EQ_INT(ipc_config_count(cfg), 0);
    ipc_config_free(cfg);

    cfg = NULL;
    UT_EQ_INT(parse_ok("# nothing here\n\n   \n\t\n", &cfg), IPC_OK);
    UT_EQ_INT(ipc_config_count(cfg), 0);
    ipc_config_free(cfg);
}
