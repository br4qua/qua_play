#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dbus/dbus.h>
#include <signal.h>

#define MPRIS_BUS_NAME "org.mpris.MediaPlayer2.qua"
#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define MPRIS_INTERFACE "org.mpris.MediaPlayer2"
#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

static int is_playing = 0;
static char *current_title = NULL;
static char *current_artist = NULL;

static void emit_metadata_changed(DBusConnection *conn);
static void emit_playback_status_changed(DBusConnection *conn);

static void run_command_async(const char *cmd, char *const args[]) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execvp(cmd, args);
    exit(1);
  }
}

static pid_t get_qua_signals_pid(void) {
  static pid_t cached_pid = 0;
  
  // Return cached PID if valid
  if (cached_pid > 0 && kill(cached_pid, 0) == 0) {
    return cached_pid;
  }
  
  // Get PID using pidof
  FILE *fp = popen("pidof qua-signals", "r");
  if (fp == NULL) {
    return -1;
  }
  
  if (fscanf(fp, "%d", &cached_pid) != 1) {
    cached_pid = -1;
  }
  
  pclose(fp);
  return cached_pid;
}


// MPRIS COMMAND HANDLERS
static void handle_play(void) {
  pid_t pid = get_qua_signals_pid();
  if (pid > 0) {
    kill(pid, SIGCONT);
    is_playing = 1;
  }
}


static void handle_next(void) {
  pid_t pid = get_qua_signals_pid();
  if (pid > 0) {
    kill(pid, SIGUSR1);
    is_playing = 1;
  }
}

static void handle_sigusr1(int sig) {
  handle_next();
}

static void handle_previous(void) {
  pid_t pid = get_qua_signals_pid();
  if (pid > 0) {
    kill(pid, SIGUSR2);
    is_playing = 1;
  }
}

static void handle_pause(void) {
  char *args[] = {"qua-stop", NULL};
  run_command_async("qua-stop", args);
  is_playing = 0;
}

static void handle_stop(void) {
  char *args[] = {"qua-stop", NULL};
  run_command_async("qua-stop", args);
  is_playing = 0;
}

static void handle_play_pause(void) {
  if (is_playing) {
    handle_pause();
  } else {
    handle_play();
  }
}

static void send_method_reply(DBusConnection *conn, DBusMessage *msg) {
  DBusMessage *reply = dbus_message_new_method_return(msg);
  if (reply) {
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
  }
}

static void emit_metadata_changed(DBusConnection *conn) {
  DBusMessage *signal_msg;
  DBusMessageIter iter, changed_props, entry, variant, invalidated_props;
  
  signal_msg = dbus_message_new_signal("/org/mpris/MediaPlayer2",
                                       "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged");
  if (!signal_msg) return;
  
  dbus_message_iter_init_append(signal_msg, &iter);
  
  const char *interface = MPRIS_PLAYER_INTERFACE;
  dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
  
  // Changed properties - include full Metadata
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &changed_props);
  
  dbus_message_iter_open_container(&changed_props, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
  const char *metadata_key = "Metadata";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &metadata_key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a{sv}", &variant);
  
  DBusMessageIter metadata_dict, metadata_entry, metadata_val, metadata_array;
  dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "{sv}", &metadata_dict);
  
  // mpris:trackid
  dbus_message_iter_open_container(&metadata_dict, DBUS_TYPE_DICT_ENTRY, NULL, &metadata_entry);
  const char *trackid_key = "mpris:trackid";
  dbus_message_iter_append_basic(&metadata_entry, DBUS_TYPE_STRING, &trackid_key);
  dbus_message_iter_open_container(&metadata_entry, DBUS_TYPE_VARIANT, "o", &metadata_val);
  const char *trackid = "/org/mpris/MediaPlayer2/Track/1";
  dbus_message_iter_append_basic(&metadata_val, DBUS_TYPE_OBJECT_PATH, &trackid);
  dbus_message_iter_close_container(&metadata_entry, &metadata_val);
  dbus_message_iter_close_container(&metadata_dict, &metadata_entry);
  
  // xesam:title
  dbus_message_iter_open_container(&metadata_dict, DBUS_TYPE_DICT_ENTRY, NULL, &metadata_entry);
  const char *title_key = "xesam:title";
  dbus_message_iter_append_basic(&metadata_entry, DBUS_TYPE_STRING, &title_key);
  dbus_message_iter_open_container(&metadata_entry, DBUS_TYPE_VARIANT, "s", &metadata_val);
  const char *title = current_title ? current_title : "No track";
  dbus_message_iter_append_basic(&metadata_val, DBUS_TYPE_STRING, &title);
  dbus_message_iter_close_container(&metadata_entry, &metadata_val);
  dbus_message_iter_close_container(&metadata_dict, &metadata_entry);
  
  // xesam:artist
  dbus_message_iter_open_container(&metadata_dict, DBUS_TYPE_DICT_ENTRY, NULL, &metadata_entry);
  const char *artist_key = "xesam:artist";
  dbus_message_iter_append_basic(&metadata_entry, DBUS_TYPE_STRING, &artist_key);
  dbus_message_iter_open_container(&metadata_entry, DBUS_TYPE_VARIANT, "as", &metadata_val);
  dbus_message_iter_open_container(&metadata_val, DBUS_TYPE_ARRAY, "s", &metadata_array);
  const char *artist = current_artist ? current_artist : "Unknown Artist";
  dbus_message_iter_append_basic(&metadata_array, DBUS_TYPE_STRING, &artist);
  dbus_message_iter_close_container(&metadata_val, &metadata_array);
  dbus_message_iter_close_container(&metadata_entry, &metadata_val);
  dbus_message_iter_close_container(&metadata_dict, &metadata_entry);
  
  dbus_message_iter_close_container(&variant, &metadata_dict);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&changed_props, &entry);
  
  dbus_message_iter_close_container(&iter, &changed_props);
  
  // Invalidated properties (empty)
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated_props);
  dbus_message_iter_close_container(&iter, &invalidated_props);
  
  dbus_connection_send(conn, signal_msg, NULL);
  dbus_connection_flush(conn);
  dbus_message_unref(signal_msg);
}

static void emit_playback_status_changed(DBusConnection *conn) {
  DBusMessage *signal_msg;
  DBusMessageIter iter, changed_props, entry, variant, invalidated_props;
  
  signal_msg = dbus_message_new_signal("/org/mpris/MediaPlayer2",
                                       "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged");
  if (!signal_msg) return;
  
  dbus_message_iter_init_append(signal_msg, &iter);
  
  const char *interface = MPRIS_PLAYER_INTERFACE;
  dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
  
  // Changed properties - PlaybackStatus
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &changed_props);
  
  dbus_message_iter_open_container(&changed_props, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
  const char *status_key = "PlaybackStatus";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &status_key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
  const char *status = is_playing ? "Playing" : "Paused";
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &status);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&changed_props, &entry);
  
  dbus_message_iter_close_container(&iter, &changed_props);
  
  // Invalidated properties (empty)
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated_props);
  dbus_message_iter_close_container(&iter, &invalidated_props);
  
  dbus_connection_send(conn, signal_msg, NULL);
  dbus_connection_flush(conn);
  dbus_message_unref(signal_msg);
}

static DBusHandlerResult message_handler(DBusConnection *conn, DBusMessage *msg, void *user_data) {
  if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "Play")) {
    handle_play();
    emit_playback_status_changed(conn);
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "Pause")) {
    handle_pause();
    emit_playback_status_changed(conn);
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "PlayPause")) {
    handle_play_pause();
    emit_playback_status_changed(conn);
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "Stop")) {
    handle_stop();
    emit_playback_status_changed(conn);
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "Next")) {
    handle_next();
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "Previous")) {
    handle_previous();
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "UpdateMetadata")) {
    const char *new_title;
    const char *new_artist;
    if (dbus_message_get_args(msg, NULL, 
                              DBUS_TYPE_STRING, &new_title,
                              DBUS_TYPE_STRING, &new_artist,
                              DBUS_TYPE_INVALID)) {
      if (current_title) free(current_title);
      if (current_artist) free(current_artist);
      current_title = strdup(new_title);
      current_artist = strdup(new_artist);
      is_playing = 1;
      fprintf(stderr, "Updated: title='%s' artist='%s'\n", current_title, current_artist);
      emit_playback_status_changed(conn);
      emit_metadata_changed(conn);
    }
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, PROPERTIES_INTERFACE, "Get")) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    const char *prop_interface, *prop_name;
    dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &prop_interface, DBUS_TYPE_STRING, &prop_name, DBUS_TYPE_INVALID);
   
    DBusMessageIter iter, variant;
    dbus_message_iter_init_append(reply, &iter);
   
    if (strcmp(prop_name, "PlaybackStatus") == 0) {
      dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &variant);
      const char *status = is_playing ? "Playing" : "Paused";
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &status);
      dbus_message_iter_close_container(&iter, &variant);
    }
    else if (strcmp(prop_name, "Metadata") == 0) {
      dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "a{sv}", &variant);
      DBusMessageIter dict, entry, val, array;
      dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "{sv}", &dict);
      
      // mpris:trackid
      dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
      const char *trackid_key = "mpris:trackid";
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &trackid_key);
      dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &val);
      const char *trackid = "/org/mpris/MediaPlayer2/Track/1";
      dbus_message_iter_append_basic(&val, DBUS_TYPE_OBJECT_PATH, &trackid);
      dbus_message_iter_close_container(&entry, &val);
      dbus_message_iter_close_container(&dict, &entry);
      
      // xesam:title
      dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
      const char *title_key = "xesam:title";
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &title_key);
      dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &val);
      const char *title = current_title ? current_title : "No track";
      dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &title);
      dbus_message_iter_close_container(&entry, &val);
      dbus_message_iter_close_container(&dict, &entry);
      
      // xesam:artist
      dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
      const char *artist_key = "xesam:artist";
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &artist_key);
      dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &val);
      dbus_message_iter_open_container(&val, DBUS_TYPE_ARRAY, "s", &array);
      const char *artist = current_artist ? current_artist : "Unknown Artist";
      dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &artist);
      dbus_message_iter_close_container(&val, &array);
      dbus_message_iter_close_container(&entry, &val);
      dbus_message_iter_close_container(&dict, &entry);
      
      dbus_message_iter_close_container(&variant, &dict);
      dbus_message_iter_close_container(&iter, &variant);
    }
    else if (strcmp(prop_name, "CanGoPrevious") == 0) {
      dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant);
      dbus_bool_t val = TRUE;
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
      dbus_message_iter_close_container(&iter, &variant);
    }
    else {
      dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant);
      dbus_bool_t val = TRUE;
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
      dbus_message_iter_close_container(&iter, &variant);
    }

    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, PROPERTIES_INTERFACE, "GetAll")) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter iter, dict, entry, variant;
    
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *key1 = "PlaybackStatus";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key1);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    const char *status = is_playing ? "Playing" : "Paused";
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &status);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
    // Add Metadata
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *metadata_key = "Metadata";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &metadata_key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a{sv}", &variant);
    
    DBusMessageIter metadata_dict, metadata_entry, metadata_val, metadata_array;
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "{sv}", &metadata_dict);
    
    // mpris:trackid
    dbus_message_iter_open_container(&metadata_dict, DBUS_TYPE_DICT_ENTRY, NULL, &metadata_entry);
    const char *trackid_key = "mpris:trackid";
    dbus_message_iter_append_basic(&metadata_entry, DBUS_TYPE_STRING, &trackid_key);
    dbus_message_iter_open_container(&metadata_entry, DBUS_TYPE_VARIANT, "o", &metadata_val);
    const char *trackid = "/org/mpris/MediaPlayer2/Track/1";
    dbus_message_iter_append_basic(&metadata_val, DBUS_TYPE_OBJECT_PATH, &trackid);
    dbus_message_iter_close_container(&metadata_entry, &metadata_val);
    dbus_message_iter_close_container(&metadata_dict, &metadata_entry);
    
    // xesam:title
    dbus_message_iter_open_container(&metadata_dict, DBUS_TYPE_DICT_ENTRY, NULL, &metadata_entry);
    const char *title_key = "xesam:title";
    dbus_message_iter_append_basic(&metadata_entry, DBUS_TYPE_STRING, &title_key);
    dbus_message_iter_open_container(&metadata_entry, DBUS_TYPE_VARIANT, "s", &metadata_val);
    const char *title = current_title ? current_title : "No track";
    dbus_message_iter_append_basic(&metadata_val, DBUS_TYPE_STRING, &title);
    dbus_message_iter_close_container(&metadata_entry, &metadata_val);
    dbus_message_iter_close_container(&metadata_dict, &metadata_entry);
    
    // xesam:artist
    dbus_message_iter_open_container(&metadata_dict, DBUS_TYPE_DICT_ENTRY, NULL, &metadata_entry);
    const char *artist_key = "xesam:artist";
    dbus_message_iter_append_basic(&metadata_entry, DBUS_TYPE_STRING, &artist_key);
    dbus_message_iter_open_container(&metadata_entry, DBUS_TYPE_VARIANT, "as", &metadata_val);
    dbus_message_iter_open_container(&metadata_val, DBUS_TYPE_ARRAY, "s", &metadata_array);
    const char *artist = current_artist ? current_artist : "Unknown Artist";
    dbus_message_iter_append_basic(&metadata_array, DBUS_TYPE_STRING, &artist);
    dbus_message_iter_close_container(&metadata_val, &metadata_array);
    dbus_message_iter_close_container(&metadata_entry, &metadata_val);
    dbus_message_iter_close_container(&metadata_dict, &metadata_entry);
    
    dbus_message_iter_close_container(&variant, &metadata_dict);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *key2 = "CanPlay";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key2);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_bool_t can_play = TRUE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &can_play);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *key3 = "CanPause";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key3);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_bool_t can_pause = TRUE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &can_pause);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *key4 = "CanGoNext";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key4);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_bool_t can_next = TRUE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &can_next);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *key5 = "CanGoPrevious";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key5);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_bool_t can_prev = TRUE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &can_prev);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *key6 = "CanControl";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key6);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_bool_t can_control = TRUE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &can_control);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
    dbus_message_iter_close_container(&iter, &dict);
    
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_INTERFACE, "Quit") ||
           dbus_message_is_method_call(msg, MPRIS_INTERFACE, "Raise")) {
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
 
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(void) {
  signal(SIGCHLD, SIG_IGN);
  signal(SIGUSR1, handle_sigusr1);
  	
  DBusError err;
  DBusConnection *conn;
  int ret;
 
  dbus_error_init(&err);
  conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "Connection Error: %s\n", err.message);
    dbus_error_free(&err);
    return 1;
  }
 
  ret = dbus_bus_request_name(conn, MPRIS_BUS_NAME, DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "Name Error: %s\n", err.message);
    dbus_error_free(&err);
    return 1;
  }
  if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) return 1;
 
  fprintf(stderr, "qua MPRIS registered\n");
  dbus_connection_add_filter(conn, message_handler, NULL, NULL);
 
  while (dbus_connection_read_write_dispatch(conn, -1));
  return 0;
}
