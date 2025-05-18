/*
 * ProFTPD: mod_dbacl -- a module for checking access control lists in a DB
 * Copyright (c) 2011-2025 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 *
 * This is mod_dbacl, contrib software for proftpd 1.3.x and above.
 * For more information contact TJ Saunders <tj@castaglia.org>.
 *
 * $Id: mod_dbacl.c,v 1.1 2011/05/11 17:51:09 tj Exp tj $
 */

#include "conf.h"
#include "privs.h"

#define MOD_DBACL_VERSION		"mod_dbacl/0.0"

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030403
# error "ProFTPD 1.3.4rc3 or later required"
#endif

module dbacl_module;

#define DBACL_POLICY_ALLOW	1
#define DBACL_POLICY_DENY	2

static int dbacl_engine = FALSE;
static int dbacl_policy = DBACL_POLICY_ALLOW;

#define DBACL_DEFAULT_TABLE		"ftpacl"
#define DBACL_DEFAULT_PATH_COL		"path"
#define DBACL_DEFAULT_READ_COL		"read_acl"
#define DBACL_DEFAULT_WRITE_COL		"write_acl"
#define DBACL_DEFAULT_DELETE_COL	"delete_acl"
#define DBACL_DEFAULT_CREATE_COL	"create_acl"
#define DBACL_DEFAULT_MODIFY_COL	"modify_acl"
#define DBACL_DEFAULT_MOVE_COL		"move_acl"
#define DBACL_DEFAULT_VIEW_COL		"view_acl"
#define DBACL_DEFAULT_NAVIGATE_COL	"navigate_acl"

static const char *dbacl_table = DBACL_DEFAULT_TABLE;
static const char *dbacl_path_col = DBACL_DEFAULT_PATH_COL;
static const char *dbacl_read_col = DBACL_DEFAULT_READ_COL;
static const char *dbacl_write_col = DBACL_DEFAULT_WRITE_COL;
static const char *dbacl_delete_col = DBACL_DEFAULT_DELETE_COL;
static const char *dbacl_create_col = DBACL_DEFAULT_CREATE_COL;
static const char *dbacl_modify_col = DBACL_DEFAULT_MODIFY_COL;
static const char *dbacl_move_col = DBACL_DEFAULT_MOVE_COL;
static const char *dbacl_view_col = DBACL_DEFAULT_VIEW_COL;
static const char *dbacl_navigate_col = DBACL_DEFAULT_NAVIGATE_COL;

static const char *dbacl_where_clause = NULL;

/* SQLNamedConnectInfo to use, if any.  Note that it would be better if
 * mod_sql.h made the MOD_SQL_DEF_CONN_NAME macro public.
 */
static const char *dbacl_conn_name = "default";

static const char *trace_channel = "dbacl";

static cmd_rec *dbacl_cmd_create(pool *parent_pool, int argc, ...) {
  pool *cmd_pool = NULL;
  cmd_rec *cmd = NULL;
  register unsigned int i = 0;
  va_list argp;

  cmd_pool = make_sub_pool(parent_pool);
  cmd = (cmd_rec *) pcalloc(cmd_pool, sizeof(cmd_rec));
  cmd->pool = cmd_pool;

  cmd->argc = argc;
  cmd->argv = (char **) pcalloc(cmd->pool, argc * sizeof(char *));

  /* Hmmm... */
  cmd->tmp_pool = cmd->pool;

  va_start(argp, argc);
  for (i = 0; i < argc; i++)
    cmd->argv[i] = va_arg(argp, char *);
  va_end(argp);

  return cmd;
}

static char *dbacl_escape_str(pool *p, char *str) {
  cmdtable *sql_cmdtab;
  cmd_rec *sql_cmd;
  modret_t *sql_res;

  /* Find the cmdtable for the sql_escapestr command. */
  sql_cmdtab = pr_stash_get_symbol(PR_SYM_HOOK, "sql_escapestr", NULL, NULL);
  if (sql_cmdtab == NULL) {
    pr_trace_msg(trace_channel, 3, "%s",
      "error: unable to find SQL hook symbol 'sql_escapestr'");
    return str;
  }

  if (strlen(str) == 0) {
    return str;
  }

  sql_cmd = dbacl_cmd_create(p, 1, pr_str_strip(p, str));

  /* Call the handler. */
  sql_res = pr_module_call(sql_cmdtab->m, sql_cmdtab->handler, sql_cmd);

  /* Check the results. */
  if (MODRET_ISDECLINED(sql_res) ||
      MODRET_ISERROR(sql_res)) {
    pr_trace_msg(trace_channel, 3, "%s",
      "error executing 'sql_escapestring'");
    return str;
  }

  return sql_res->data;
}

static array_header *dbacl_split_path(pool *p, char *path) {
  char *dup_path, *ptr;
  size_t dup_pathlen;
  array_header *elts;

  dup_pathlen = strlen(path);
  dup_path = pstrndup(p, path, dup_pathlen);

  if (dup_pathlen == 1) {
    /* Just "/" here. */
    elts = make_array(p, 1, sizeof(char **));
    *((char **) push_array(elts)) = pstrdup(p, "/");

    return elts;
  }

  /* If the last character is a path separator, trim it off. */
  if (dup_path[dup_pathlen-1] == '/') {
    dup_path[dup_pathlen-1] = '\0';
    dup_pathlen--;
  }

  /* Start looking for path separators, just past the first character. */
  ptr = strchr(dup_path + 1, '/');
  if (ptr == NULL) {
    pr_trace_msg(trace_channel, 2,
      "unable to split path '%s': no usable path separators found", path);
    errno = EINVAL;
    return NULL;
  }

  /* The goal here is to a split a path like:
   *
   *  /home/user/dir/file.txt
   *
   * Into a list of paths like so:
   *
   *  /home
   *  /home/user
   *  /home/user/dir
   *  /home/user/dir/file.txt
   */

  elts = make_array(p, 2, sizeof(char **));

  while (ptr != NULL) {
    *ptr = '\0';

    pr_signals_handle();

    *((char **) push_array(elts)) = pstrdup(p, dup_path);
    *ptr = '/';

    ptr = strchr(ptr + 1, '/');
  }

  /* And don't forget the full path itself. */
  *((char **) push_array(elts)) = pstrdup(p, dup_path);

  return elts;
}

static char *dbacl_get_path_skip_opts(cmd_rec *cmd) {
  char *ptr, *path = NULL;

  if (cmd->arg == NULL) {
    errno = ENOENT;
    return NULL;
  }

  ptr = path = cmd->arg;

  while (isspace((int) *ptr)) {
    pr_signals_handle();
    ptr++;
  }

  if (*ptr == '-') {
    /* Options are found; skip past the leading whitespace. */
    path = ptr;
  }

  while (path &&
         *path == '-') {

    /* Advance to the next whitespace */
    while (*path != '\0' &&
           !isspace((int) *path)) {
      path++;
    }

    ptr = path;

    while (*ptr &&
           isspace((int) *ptr)) {
      pr_signals_handle();
      ptr++;
    }

    if (*ptr == '-') {
      /* Options are found; skip past the leading whitespace. */
      path = ptr;

    } else if (*(path + 1) == ' ') {
      /* If the next character is a blank space, advance just one character. */
      path++;
      break;

    } else {
      path = ptr;
      break;
    }
  }

  return path;
}

static char *dbacl_get_path(cmd_rec *cmd, const char *proto) {
  char *path = NULL, *abs_path = NULL;

  if (strncasecmp(proto, "ftp", 4) == 0 ||
      strncasecmp(proto, "ftps", 5) == 0) {
    if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) == 0) {
        if (strncasecmp(cmd->argv[1], "CHMOD", 6) == 0 ||
            strncasecmp(cmd->argv[1], "CHGRP", 6) == 0) {
          register unsigned int i;

          path = "";
          for (i = 3; i < cmd->argc; i++) {
            path = pstrcat(cmd->tmp_pool, path, *path ? " " : "", cmd->argv[i],
              NULL);
          }
        }

    } else if (pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
               pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0) {
      path = dbacl_get_path_skip_opts(cmd);

    } else if (pr_cmd_cmp(cmd, PR_CMD_PWD_ID) == 0 ||
               pr_cmd_cmp(cmd, PR_CMD_XPWD_ID) == 0) {
      path = (char *) pr_fs_getcwd();

    } else {
      path = cmd->arg;
    }

  } else if (strncasecmp(proto, "sftp", 5) == 0) {
    path = cmd->arg;

  } else {
    pr_trace_msg(trace_channel, 1,
      "unable to get path from command: unsupported protocol '%s'", proto);
    errno = EINVAL;
    return NULL;
  }

  abs_path = dir_abs_path(cmd->tmp_pool, path, TRUE);
  if (abs_path == NULL) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 1, "error resolving '%s': %s", path,
      strerror(xerrno));

    errno = EINVAL;
    return NULL;
  }

  pr_trace_msg(trace_channel, 17, "resolved path '%s' to '%s'", path, abs_path);
  return abs_path;
}

static const char *dbacl_get_column(cmd_rec *cmd, const char *proto) {
  const char *col = NULL;

  if (pr_cmd_cmp(cmd, PR_CMD_RETR_ID) == 0) {
    col = dbacl_read_col;

  } else if (pr_cmd_cmp(cmd, PR_CMD_STOR_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_APPE_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_STOU_ID) == 0) {
    col = dbacl_write_col;

  } else if (pr_cmd_cmp(cmd, PR_CMD_DELE_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_RMD_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_XRMD_ID) == 0) {
    col = dbacl_delete_col;

  } else if (pr_cmd_cmp(cmd, PR_CMD_MKD_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_XMKD_ID) == 0) {
    col = dbacl_create_col;

  } else if (pr_cmd_cmp(cmd, PR_CMD_MFMT_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_MFF_ID) == 0) {
    col = dbacl_modify_col;

  } else if (pr_cmd_cmp(cmd, PR_CMD_RNFR_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_RNTO_ID) == 0) {
    col = dbacl_move_col;

  } else if (pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_MLSD_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_MLST_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_STAT_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_MDTM_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_SIZE_ID) == 0) {
    col = dbacl_view_col;

  } else if (pr_cmd_cmp(cmd, PR_CMD_CDUP_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_XCUP_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_CWD_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_XCWD_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_PWD_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_XPWD_ID) == 0) {
    col = dbacl_navigate_col;

  } else if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) == 0) {
    if (strncasecmp(cmd->argv[1], "CHMOD", 6) == 0 ||
        strncasecmp(cmd->argv[1], "CHGRP", 6) == 0) {
      col = dbacl_modify_col;

    } else if (strncasecmp(cmd->argv[1], "CPTO", 5) == 0) {
      col = dbacl_move_col;
    }

  } else if (strncasecmp(proto, "sftp", 5) == 0) {
    /* Mapping of SFTP requests to ACLs */

    if (pr_cmd_strcmp(cmd, "LSTAT") == 0 ||
        pr_cmd_strcmp(cmd, "OPENDIR") == 0 ||
        pr_cmd_strcmp(cmd, "READLINK") == 0) {
      col = dbacl_view_col;

    } else if (pr_cmd_strcmp(cmd, "REALPATH") == 0) {
      col = dbacl_navigate_col;

    } else if (pr_cmd_strcmp(cmd, "SETSTAT") == 0 ||
               pr_cmd_strcmp(cmd, "FSETSTAT") == 0) {
      col = dbacl_modify_col;

    } else if (pr_cmd_strcmp(cmd, "RENAME") == 0) {
      col = dbacl_move_col;

    } else if (pr_cmd_strcmp(cmd, "SYMLINK") == 0 ||
               pr_cmd_strcmp(cmd, "LINK") == 0) {
      col = dbacl_create_col;
    }
  }

  return col;
}

static int dbacl_is_boolean(const char *str) {
  int res;

  res = pr_str_is_boolean(str);
  if (res < 0) {
    if (errno == EINVAL) {
      /* Treat 'allow(ed)' and 'den(y|ied)' as acceptable Boolean values
       * as well.
       */
      if (strncasecmp(str, "allow", 6) == 0 ||
          strncasecmp(str, "allowed", 8) == 0) {
        res = TRUE;

      } else if (strncasecmp(str, "deny", 5) == 0 ||
                 strncasecmp(str, "denied", 7) == 0) {
        res = FALSE;

      } else {
        pr_trace_msg(trace_channel, 6,
          "unable to interpret database value '%s' as Boolean value", str);
      }
    }
  }

  return res;
}

static int dbacl_get_row(pool *p, const char *acl_col,
    array_header *path_elts) {
  register unsigned int i;
  cmd_rec *sql_cmd = NULL;
  char *query = NULL, *query_name = NULL, **elts, **values;
  array_header *list_elts, *sql_data = NULL;
  cmdtable *sql_cmdtab = NULL;
  modret_t *sql_res = NULL;

  /* Sanitize the path components in the list we'll be used, to avoid any
   * SQL injection attacks.
   */
  list_elts = make_array(p, path_elts->nelts, sizeof(char **));
  elts = path_elts->elts;
  for (i = 0; i < path_elts->nelts; i++) {
    *((char **) push_array(list_elts)) = dbacl_escape_str(p, elts[i]);
  }

  /* SQL query to use:
   *
   *  SELECT acl_col FROM dbacl_table
   *    WHERE
   *      path_col IN ($list)
   *      ORDER BY LENGTH(path_col)
   *      DESC LIMIT 1;
   *
   * Expand "($list)" to e.g.
   *
   *   ('/home', '/home/user', '/home/user/dir', '/home/user/dir/file.txt')
   */

  /* Default schema:
   *
   *  CREATE TABLE dbacl (
   *    user VARCHAR NOT NULL,
   *    group VARCHAR NOT NULL,
   *    path VARCHAR NOT NULL,
   *    create BOOLEAN,
   *    modify BOOLEAN,
   *    write BOOLEAN,
   *    read BOOLEAN,
   *    delete BOOLEAN,
   *    move BOOLEAN,
   *    view BOOLEAN,
   *  );
   */

  /* Find the cmdtable for the sql_lookup command. */
  sql_cmdtab = pr_stash_get_symbol(PR_SYM_HOOK, "sql_lookup", NULL, NULL);
  if (sql_cmdtab == NULL) {
    pr_trace_msg(trace_channel, 3, "%s",
      "error: unable to find SQL hook symbol 'sql_lookup'");
    errno = EPERM;
    return -1;
  }

  /* Build up the query to use, including WHERE clause. */
  query = pstrcat(p, acl_col, " FROM ", dbacl_table, " WHERE ", NULL);

  if (dbacl_where_clause != NULL) {
    query = pstrcat(p, query, "(", dbacl_where_clause, ") AND ", NULL);
  }

  query = pstrcat(p, query, dbacl_path_col, " IN (", NULL);

  elts = list_elts->elts;
  for (i = 0; i < list_elts->nelts; i++) {
    query = pstrcat(p, query, "'", elts[i], "'", NULL);

    if (i != (list_elts->nelts-1)) {
      /* Only append the comma separator if we are not the last item in the
       * list.
       */
      query = pstrcat(p, query, ", ", NULL);
    }
  }

  query = pstrcat(p, query, ") ORDER BY LENGTH(", dbacl_path_col,
    ") DESC LIMIT 1", NULL);

  pr_trace_msg(trace_channel, 7, "constructed query '%s'", query);

  /* Cheat, and programmatically create a SQLNamedQuery for this query. */
  query_name = pstrcat(p, "SQLNamedQuery_", MOD_DBACL_VERSION, NULL);

  add_config_param_set(&(main_server->conf), query_name, 3, "SELECT", query,
    dbacl_conn_name);

  sql_cmd = dbacl_cmd_create(p, 2, "sql_lookup", MOD_DBACL_VERSION);

  /* Call the handler. */
  sql_res = pr_module_call(sql_cmdtab->m, sql_cmdtab->handler, sql_cmd);

  /* Check the results. */
  if (MODRET_ISDECLINED(sql_res) ||
      MODRET_ISERROR(sql_res)) {
    pr_trace_msg(trace_channel, 2,
      "error processing SQL query '%s', check SQLLogFile for details", query);
    errno = EPERM;
    return -1;
  }

  /* Remove the SQLNamedQuery. */
  (void) remove_config(main_server->conf, query_name, FALSE);

  sql_data = (array_header *) sql_res->data;

  if (sql_data->nelts == 0) {
    pr_trace_msg(trace_channel, 8, "query '%s' returned no matching rows",
      query);
    errno = ENOENT;
    return -1;
  }

  if (sql_data->nelts != 1) {
    pr_trace_msg(trace_channel, 5,
      "query '%s' returned incorrect number of values (%d)", query,
      sql_data->nelts);
    errno = EINVAL;
    return -1;
  }

  values = (char **) sql_data->elts;

  pr_trace_msg(trace_channel, 8,
    "query '%s' returned value '%s'", query, values[0]);

  return dbacl_is_boolean(values[0]);
}

static int dbacl_get_path_acl(cmd_rec *cmd, const char *acl_col, char *path,
    int *policy) {
  array_header *path_elts;
  int res;

  path_elts = dbacl_split_path(cmd->tmp_pool, path);
  if (path_elts == NULL) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 4,
      "error splitting path '%s': %s", path, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  if (pr_trace_get_level(trace_channel) >= 9) {
    register unsigned int i;
    char **elts;

    pr_trace_msg(trace_channel, 9,
      "split path '%s' into the following list:", path);

    elts = path_elts->elts;
    for (i = 0; i < path_elts->nelts; i++) {
      pr_trace_msg(trace_channel, 9,
        "path component #%u: '%s'", i+1, elts[i]);
    }
  }

  res = dbacl_get_row(cmd->tmp_pool, acl_col, path_elts);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 4,
      "error getting database row for ACL column '%s', path '%s': %s",
      acl_col, path, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  if (res == FALSE) {
    char *cmd_name;

    cmd_name = cmd->argv[0];
    if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) == 0) {
      cmd_name = pstrcat(cmd->tmp_pool, cmd->argv[0], " ", cmd->argv[1], NULL);
    }

    pr_trace_msg(trace_channel, 9,
      "command '%s' on path '%s' explicitly denied by table '%s', column '%s'",
      cmd_name, path, dbacl_table, acl_col);

    *policy = DBACL_POLICY_DENY;

  } else {
    char *cmd_name;

    cmd_name = cmd->argv[0];
    if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) == 0) {
      cmd_name = pstrcat(cmd->tmp_pool, cmd->argv[0], " ", cmd->argv[1], NULL);
    }

    pr_trace_msg(trace_channel, 9,
      "command '%s' on path '%s' explicitly allowed by table '%s', column '%s'",
      cmd_name, path, dbacl_table, acl_col);

    *policy = DBACL_POLICY_ALLOW;
  }

  return res;
}

static int dbacl_get_acl(cmd_rec *cmd, const char *proto, int *policy) {
  const char *acl_col;
  char *path;
  int res;

  acl_col = dbacl_get_column(cmd, proto);
  if (acl_col == NULL) {
    pr_trace_msg(trace_channel, 4,
      "no mapping of command '%s' to ACL column", cmd->argv[0]);
    return -1;
  }

  /* We need to handle SFTP LINK/SYMLINK requests differently, since
   * they a) involve two paths and b) have no direct FTP equivalents.
   *
   * XXX What about mod_site_misc's SITE SYMLINK?
   */

  if (strncasecmp(proto, "ftp", 4) == 0 ||
      strncasecmp(proto, "ftps", 5) == 0 ) {
    path = dbacl_get_path(cmd, proto);
    if (path == NULL) {
      pr_trace_msg(trace_channel, 4,
        "unable to get full path for command '%s'", cmd->argv[0]);
      return -1;
    }

    res = dbacl_get_path_acl(cmd, acl_col, path, policy);
    if (res < 0) {
      return res;
    }

    return 0;

  } else if (strncasecmp(proto, "sftp", 5) == 0) {
    if (pr_cmd_strcmp(cmd, "SYMLINK") != 0 &&
        pr_cmd_strcmp(cmd, "LINK") != 0) {

      path = dbacl_get_path(cmd, proto);
      if (path == NULL) {
        pr_trace_msg(trace_channel, 4,
          "unable to get full path for command '%s'", cmd->argv[0]);
        return -1;
      }

      res = dbacl_get_path_acl(cmd, acl_col, path, policy);
      if (res < 0) {
        return res;
      }

      return 0;

    } else {
      char *arg, *ptr;

      arg = pstrdup(cmd->tmp_pool, cmd->arg);
      ptr = strchr(arg, '\t');
      if (ptr == NULL) {
        /* Malformed SFTP SYMLINK/LINK cmd_rec. */
        pr_trace_msg(trace_channel, 1,
          "malformed SFTP %s request, ignoring", cmd->argv[0]);
        errno = EINVAL;
        return -1;
      }

      *ptr = '\0';

      /* Check source path first. */
      path = arg;

      /* XXX Need to absolutize this path */

      res = dbacl_get_path_acl(cmd, acl_col, path, policy);
      if (res < 0) {
        return res;
      }

      if (*policy == DBACL_POLICY_DENY) {
        /* Source path is denied; reject the request. */
        return 0;
      }

      /* Check destination path next. */
      path = ptr + 1;

      /* XXX Need to absolutize this path */

      res = dbacl_get_path_acl(cmd, acl_col, path, policy);
      if (res < 0) {
        return res;
      }

      return 0;
    }
  }

  errno = ENOSYS;
  return -1;
}

static void dbacl_set_error_response(cmd_rec *cmd, const char *msg) {

  /* Command-specific error code/message */
  if (pr_cmd_cmp(cmd, PR_CMD_CDUP_ID) == 0 ||
      pr_cmd_cmp(cmd, PR_CMD_XCUP_ID) == 0) {
    pr_response_add_err(R_550, "%s", msg);

  } else if (pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0) {
    size_t arglen;

    /* We have may received bare LIST/NLST commands, or just options and no
     * paths.  Do The Right Thing(tm) with these scenarios.
     */

    arglen = strlen(cmd->arg);
    if (arglen == 0) {
      /* No options, no path. */
      pr_response_add_err(R_450, ".: %s", msg);

    } else {
      char *path;

      path = dbacl_get_path_skip_opts(cmd);

      arglen = strlen(path);
      if (arglen == 0) {
        /* Only options, no path. */
        pr_response_add_err(R_450, ".: %s", msg);

      } else {
        pr_response_add_err(R_450, "%s: %s", cmd->arg, msg);
      }
    }

  } else if (pr_cmd_cmp(cmd, PR_CMD_MFMT_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_MFF_ID) == 0) {
    pr_response_add_err(R_550, "%s: %s", cmd->argv[2], msg);

  } else if (pr_cmd_cmp(cmd, PR_CMD_MLSD_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_MLST_ID) == 0) {
    size_t arglen;

    arglen = strlen(cmd->arg);
    if (arglen == 0) {

      /* No path. */
      pr_response_add_err(R_550, ".: %s", msg);

    } else {
      pr_response_add_err(R_550, "%s: %s", cmd->arg, msg);
    }

  } else if (pr_cmd_cmp(cmd, PR_CMD_STAT_ID) == 0) {
    size_t arglen;

    arglen = strlen(cmd->arg);
    if (arglen == 0) {

      /* No path. */
      pr_response_add_err(R_550, "%s", msg);

    } else {
      pr_response_add_err(R_550, "%s: %s", cmd->arg, msg);
    }

  } else if (pr_cmd_cmp(cmd, PR_CMD_PWD_ID) == 0 ||
             pr_cmd_cmp(cmd, PR_CMD_XPWD_ID) == 0) {
    pr_response_add_err(R_550, "%s", msg);

  } else if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) == 0) {
    register unsigned int i;
    char *arg = "";

    if (strncasecmp(cmd->argv[1], "CHMOD", 6) == 0 ||
        strncasecmp(cmd->argv[1], "CHGRP", 6) == 0) {

      for (i = 3; i < cmd->argc; i++) {
        arg = pstrcat(cmd->tmp_pool, arg, *arg ? " " : "", cmd->argv[i], NULL);
      }

    } else {
      /* XXX Refine this case for other SITE commands. */

      for (i = 2; i < cmd->argc; i++) {
        arg = pstrcat(cmd->tmp_pool, arg, *arg ? " " : "", cmd->argv[i], NULL);
      }
    }

    pr_response_add_err(R_550, "%s: %s", arg, msg);

  } else {
    pr_response_add_err(R_550, "%s: %s", cmd->arg, msg);
  }
}

/* XXX NOTES:
 *
 *  Look up relevant row, using _escaped_ path, acl name, uid/user, gid/group
 *    Hint: index on the lookup columns: path, and columns in WHERE clause.
 */

/* Configuration handlers
 */

/* usage: DBACLEngine on|off */
MODRET set_dbaclengine(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  bool = get_boolean(cmd, 1);
  if (bool == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = bool;

  return PR_HANDLED(cmd);
}

/* usage: DBACLPolicy policy */
MODRET set_dbaclpolicy(cmd_rec *cmd) {
  config_rec *c;
  int policy;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (strncasecmp(cmd->argv[1], "allow", 6) == 0) {
    policy = DBACL_POLICY_ALLOW;

  } else if (strncasecmp(cmd->argv[1], "deny", 5) == 0) {
    policy = DBACL_POLICY_DENY;

  } else {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown DBACLPolicy '",
      cmd->argv[1], "'", NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = policy;

  return PR_HANDLED(cmd);
}

/* usage: DBACLSchema table [cols] [conn-name] */
MODRET set_dbaclschema(cmd_rec *cmd) {

  if (cmd->argc-1 != 1 &&
      cmd->argc-1 != 10 &&
      cmd->argc-1 != 11) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (cmd->argc-1 == 1) {
    /* Just the table name. */
    (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
    return PR_HANDLED(cmd);

  } else if (cmd->argc-1 == 10) {
    /* Table name and column names. */
    (void) add_config_param_str(cmd->argv[0], 10, cmd->argv[1], cmd->argv[2],
      cmd->argv[3], cmd->argv[4], cmd->argv[5], cmd->argv[6], cmd->argv[7],
      cmd->argv[8], cmd->argv[9], cmd->argv[10]);

  } else if (cmd->argc-1 == 11) {

    /* Table name, column names, and connection name. */
    (void) add_config_param_str(cmd->argv[0], 11, cmd->argv[1], cmd->argv[2],
      cmd->argv[3], cmd->argv[4], cmd->argv[5], cmd->argv[6], cmd->argv[7],
      cmd->argv[8], cmd->argv[9], cmd->argv[10], cmd->argv[11]);
  }

  return PR_HANDLED(cmd);
}

/* usage: DBACLWhereClause clause */
MODRET set_dbaclwhereclause(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* Command handlers
 */

MODRET dbacl_pre_cmd(cmd_rec *cmd) {
  int policy, res;
  const char *proto;

  if (!dbacl_engine) {
    return PR_DECLINED(cmd);
  }

  proto = pr_session_get_protocol(0);

  res = dbacl_get_acl(cmd, proto, &policy);
  if (res < 0) {
    if (dbacl_policy == DBACL_POLICY_DENY) {
      pr_trace_msg(trace_channel, 3,
        "error looking up ACL for %s command/resource (protocol '%s') "
        "and 'DBACLPolicy deny' setting in effect, rejecting command",
        cmd->argv[0], proto);

      dbacl_set_error_response(cmd, strerror(EACCES));
      errno = EACCES;
      return PR_ERROR(cmd);
    }

    return PR_DECLINED(cmd);
  }

  if (policy == DBACL_POLICY_DENY) {
    pr_trace_msg(trace_channel, 3,
      "configured ACL for %s command/resource (protocol '%s') denies "
      "access, rejecting command", cmd->argv[0], proto);

    dbacl_set_error_response(cmd, strerror(EACCES));
    errno = EACCES;
    return PR_ERROR(cmd);
  }

  pr_trace_msg(trace_channel, 9,
    "configured ACL for %s command/resource (protocol '%s') allows access, "
    "permitting command", cmd->argv[0], proto);

  return PR_DECLINED(cmd);
}

MODRET dbacl_post_pass(cmd_rec *cmd) {
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "DBACLEngine", FALSE);
  if (c) {
    dbacl_engine = *((int *) c->argv[0]);
  }

  if (!dbacl_engine) {
    return PR_DECLINED(cmd);
  }

  c = find_config(main_server->conf, CONF_PARAM, "DBACLSchema", FALSE);
  if (c) {
    if (c->argc == 1) {
      dbacl_table = c->argv[0];

    } else if (c->argc >= 10) {
      dbacl_table = c->argv[0];

      dbacl_path_col = c->argv[1];
      dbacl_read_col = c->argv[2];
      dbacl_write_col = c->argv[3];
      dbacl_delete_col = c->argv[4];
      dbacl_create_col = c->argv[5];
      dbacl_modify_col = c->argv[6];
      dbacl_move_col = c->argv[7];
      dbacl_view_col = c->argv[8];
      dbacl_navigate_col = c->argv[9];

      if (c->argc == 11) {
        dbacl_conn_name = c->argv[10];
      }
    }
  }

  if (pr_trace_get_level(trace_channel) >= 15) {
    pr_trace_msg(trace_channel, 15,
      "using table name '%s' for ACLs", dbacl_table);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for paths", dbacl_path_col);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for the READ ACL", dbacl_read_col);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for the WRITE ACL", dbacl_write_col);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for the DELETE ACL", dbacl_delete_col);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for the CREATE ACL", dbacl_create_col);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for the MODIFY ACL", dbacl_modify_col);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for the MOVE ACL", dbacl_move_col);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for the VIEW ACL", dbacl_view_col);

    pr_trace_msg(trace_channel, 15,
      "using column name '%s' for the NAVIGATE ACL", dbacl_navigate_col);
  }

  c = find_config(main_server->conf, CONF_PARAM, "DBACLPolicy", FALSE);
  if (c) {
    dbacl_policy = *((int *) c->argv[0]);
  }

  c = find_config(main_server->conf, CONF_PARAM, "DBACLWhereClause", FALSE);
  if (c) {
    dbacl_where_clause = c->argv[0];
  }

  return PR_DECLINED(cmd);
}

/* Module API tables
 */

static conftable dbacl_conftab[] = {
  { "DBACLEngine",	set_dbaclengine,	NULL },
  { "DBACLPolicy",	set_dbaclpolicy,	NULL },
  { "DBACLSchema",	set_dbaclschema,	NULL },
  { "DBACLWhereClause",	set_dbaclwhereclause,	NULL },

  { NULL }
};

static cmdtable dbacl_cmdtab[] = {
  { PRE_CMD,	C_APPE,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,    C_CDUP, G_NONE, dbacl_pre_cmd,          TRUE,   FALSE },
  { PRE_CMD,    C_XCUP, G_NONE, dbacl_pre_cmd,          TRUE,   FALSE },
  { PRE_CMD,    C_CWD,  G_NONE, dbacl_pre_cmd,          TRUE,   FALSE },
  { PRE_CMD,    C_XCWD, G_NONE, dbacl_pre_cmd,          TRUE,   FALSE },
  { PRE_CMD,	C_DELE,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_LIST,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_MDTM,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_MFF,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_MFMT,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_MKD,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_XMKD,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_MLSD,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_MLST,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_NLST,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_PWD,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_XPWD,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_RETR,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_RMD,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_XRMD,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_RNFR,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_RNTO,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_SITE,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_SIZE,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_STAT,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_STOR,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },
  { PRE_CMD,	C_STOU,	G_NONE,	dbacl_pre_cmd,		TRUE,	FALSE },

  /* SFTP requests */
  { PRE_CMD,	"FSETSTAT",	G_NONE,	dbacl_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"LINK",		G_NONE,	dbacl_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"LSTAT",	G_NONE,	dbacl_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"OPENDIR",	G_NONE,	dbacl_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"READLINK",	G_NONE,	dbacl_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"REALPATH",	G_NONE,	dbacl_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"SETSTAT",	G_NONE,	dbacl_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"SYMLINK",	G_NONE,	dbacl_pre_cmd,	TRUE,	FALSE },

  /* XXX Need to handle SITE CPFR, SITE CPTO, SFTP COPY */

  { POST_CMD,	C_PASS,	G_NONE,	dbacl_post_pass,	FALSE,	FALSE },

  { 0, NULL }
};

module dbacl_module = {
  NULL, NULL,

  /* Module API version 2.0 */
  0x20,

  /* Module name */
  "dbacl",

  /* Module configuration handler table */
  dbacl_conftab,

  /* Module command handler table */
  dbacl_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization function */
  NULL,

  /* Session initialization function */
  NULL,

  /* Module version */
  MOD_DBACL_VERSION
};
