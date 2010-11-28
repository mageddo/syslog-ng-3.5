/*
 * Copyright (c) 2002-2010 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2010 Balázs Scheidler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "dbparser.h"
#include "patterndb.h"
#include "radix.h"
#include "apphook.h"

#include <sys/stat.h>

struct _LogDBParser
{
  LogParser super;
  PatternDB *db;
  GlobalConfig *cfg;
  guint timer_tick_id;
  gchar *db_file;
  time_t db_file_last_check;
  ino_t db_file_inode;
  time_t db_file_mtime;
};

static void
log_db_parser_emit(LogMessage *msg, gboolean synthetic, gpointer user_data)
{
  if (synthetic)
    {
      msg_post_message(msg);
      msg_debug("db-parser: emitting synthetic message",
                evt_tag_str("msg", log_msg_get_value(msg, LM_V_MESSAGE, NULL)),
                NULL);
    }
}

static void
log_db_parser_reload_database(LogDBParser *self)
{
  struct stat st;
  PatternDB *db_new;

  if (stat(self->db_file, &st) < 0)
    {
      msg_error("Error stating pattern database file, no automatic reload will be performed",
                evt_tag_str("error", g_strerror(errno)),
                NULL);
      return;
    }
  if ((self->db_file_inode == st.st_ino && self->db_file_mtime == st.st_mtime))
    {
      return;
    }

  self->db_file_inode = st.st_ino;
  self->db_file_mtime = st.st_mtime;

  db_new = pattern_db_new();
  if (!pattern_db_load(db_new, self->cfg, self->db_file, NULL))
    {
      msg_error("Error reloading pattern database, no automatic reload will be performed", NULL);
      /* restore the old object pointers */
      pattern_db_free(db_new);
    }
  else
    {
      /* free the old database if the new was loaded successfully */
      if (self->db)
        pattern_db_free(self->db);
      self->db = db_new;
      pattern_db_set_emit_func(self->db, log_db_parser_emit, self);

      msg_notice("Log pattern database reloaded",
                 evt_tag_str("file", self->db_file),
                 evt_tag_str("version", self->db->version),
                 evt_tag_str("pub_date", self->db->pub_date),
                 NULL);
    }

}

static gboolean
log_db_parser_timer_tick(gpointer s)
{
  LogDBParser *self = (LogDBParser *) s;

  pattern_db_timer_tick(self->db);
  return TRUE;
}

static gchar *
log_db_parser_format_persist_name(LogDBParser *self)
{
  static gchar persist_name[512];

  g_snprintf(persist_name, sizeof(persist_name), "db-parser(%s)", self->db_file);
  return persist_name;
}

static gboolean
log_db_parser_init(LogPipe *s)
{
  LogDBParser *self = (LogDBParser *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  self->db = cfg_persist_config_fetch(cfg, log_db_parser_format_persist_name(self));
  if (self->db)
    {
      struct stat st;

      if (stat(self->db_file, &st) < 0)
        {
          msg_error("Error stating pattern database file, no automatic reload will be performed",
                    evt_tag_str("error", g_strerror(errno)),
                    NULL);
        }
      else
        {
          self->db_file_inode = st.st_ino;
          self->db_file_mtime = st.st_mtime;
        }
    }
  else
    {
      log_db_parser_reload_database(self);
    }

  self->timer_tick_id = g_timeout_add_seconds(1, log_db_parser_timer_tick, self);
  return self->db != NULL;
}

static gboolean
log_db_parser_deinit(LogPipe *s)
{
  LogDBParser *self = (LogDBParser *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  if (self->timer_tick_id)
    {
      GSource *source = g_main_context_find_source_by_id(NULL, self->timer_tick_id);

      g_source_destroy(source);
      self->timer_tick_id = 0;
    }

  cfg_persist_config_add(cfg, log_db_parser_format_persist_name(self), self->db, (GDestroyNotify) pattern_db_free, FALSE);
  self->db = NULL;
  return TRUE;
}

static gboolean
log_db_parser_process(LogParser *s, LogMessage *msg, const char *input)
{
  LogDBParser *self = (LogDBParser *) s;

  if (G_UNLIKELY(self->db_file_last_check == 0 || self->db_file_last_check < msg->timestamps[LM_TS_RECVD].time.tv_sec - 5))
    {
      self->db_file_last_check = msg->timestamps[LM_TS_RECVD].time.tv_sec;
      log_db_parser_reload_database(self);
    }
  if (self->db)
    pattern_db_process(self->db, msg, NULL);
  return TRUE;
}

void
log_db_parser_set_db_file(LogDBParser *self, const gchar *db_file)
{
  if (self->db_file)
    g_free(self->db_file);
  self->db_file = g_strdup(db_file);
}

static LogPipe *
log_db_parser_clone(LogProcessPipe *s)
{
  msg_error("It is not supported to reference a single db-parser() instance from multiple locations of the configuration file. Please create separate db-parser() parsers for that purpose.", NULL);
  return NULL;
}

static void
log_db_parser_free(LogPipe *s)
{
  LogDBParser *self = (LogDBParser *) s;

  if (self->db)
    pattern_db_free(self->db);

  if (self->db_file)
    g_free(self->db_file);
  log_parser_free_method(s);
}

LogParser *
log_db_parser_new(void)
{
  LogDBParser *self = g_new0(LogDBParser, 1);

  log_parser_init_instance(&self->super);
  self->super.super.super.free_fn = log_db_parser_free;
  self->super.super.super.init = log_db_parser_init;
  self->super.super.super.deinit = log_db_parser_deinit;
  self->super.super.clone = log_db_parser_clone;
  self->super.process = log_db_parser_process;
  self->db_file = g_strdup(PATH_PATTERNDB_FILE);
  self->cfg = configuration;

  return &self->super;
}
