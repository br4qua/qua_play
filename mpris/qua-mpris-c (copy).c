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

static void emit_properties_changed(DBusConnection *conn) {
  DBusMessage *signal_msg;
  DBusMessageIter iter, changed_props, entry, variant, invalidated_props;
  
  signal_msg = dbus_message_new_signal("/org/mpris/MediaPlayer2",
                                       "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged");
  
  dbus_message_iter_init_append(signal_msg, &iter);
  
  // Interface name
  const char *interface = MPRIS_PLAYER_INTERFACE;
  dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
  
  // Changed properties - include the actual Metadata
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &changed_props);
  
  dbus_message_iter_open_container(&changed_props, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
  const char *metadata_key = "Metadata";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &metadata_key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a{sv}", &variant);
  append_metadata(&variant);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&changed_props, &entry);
  
  dbus_message_iter_close_container(&iter, &changed_props);
  
  // Invalidated properties (empty now since we're sending the actual value)
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated_props);
  dbus_message_iter_close_container(&iter, &invalidated_props);
  
  dbus_connection_send(conn, signal_msg, NULL);
  dbus_message_unref(signal_msg);
}

static void run_command_async(const char *cmd, char *const args[]) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execvp(cmd, args);
    exit(1);
  }
}

// MPRIS COMMAND HANDLERS
static void handle_play(void) {
  char *args[] = {"qua-play", NULL};
  run_command_async("qua-play", args);
  is_playing = 1;
}

static void handle_next(void) {
  char *args[] = {"qua-play", "-n", "1", NULL};
  run_command_async("qua-play", args);
  is_playing = 1;
}

static void handle_sigusr1(int sig) {
  handle_next();
}

static void handle_previous(void) {
  char *args[] = {"qua-play", "-n", "-1", NULL};
  run_command_async("qua-play", args);
  is_playing = 1;
}

static void handle_pause(void) {
  char *args[] = {"qua-play", NULL};
  run_command_async("qua-play", args);
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

static void append_metadata(DBusMessageIter *iter) {
  DBusMessageIter dict, entry, variant, array;
  
  // Open the a{sv} container for metadata
  dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
  
  // Add mpris:trackid (required by spec)
  dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
  const char *trackid_key = "mpris:trackid";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &trackid_key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &variant);
  const char *trackid_value = "/org/mpris/MediaPlayer2/Track/1";
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &trackid_value);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&dict, &entry);
  
  // Add xesam:title
  dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
  const char *title_key = "xesam:title";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &title_key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
  const char *title_to_show = current_title ? current_title : "No track";
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &title_to_show);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&dict, &entry);
  
  // Add xesam:artist (as string array)
  dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
  const char *artist_key = "xesam:artist";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &artist_key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
  dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array);
  const char *artist_value = "xesam:artist";
  dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &artist_value);
  dbus_message_iter_close_container(&variant, &array);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&dict, &entry);
  
  // Add xesam:album
  dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
  const char *album_key = "xesam:album";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &album_key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
  const char *album_value = "xesam:album";
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &album_value);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&dict, &entry);
  
  // Add mpris:length (in microseconds)
  dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
  const char *length_key = "mpris:length";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &length_key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "x", &variant);
  int64_t length_value = 180000000; // 3 minutes in microseconds
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT64, &length_value);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&dict, &entry);
  
  dbus_message_iter_close_container(iter, &dict);
}

static DBusHandlerResult message_handler(DBusConnection *conn, DBusMessage *msg, void *user_data) {
  if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "Play")) {
    handle_play();
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "Pause")) {
    handle_pause();
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "PlayPause")) {
    handle_play_pause();
    send_method_reply(conn, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  else if (dbus_message_is_method_call(msg, MPRIS_PLAYER_INTERFACE, "Stop")) {
    handle_stop();
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
    if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &new_title, DBUS_TYPE_INVALID)) {
      free(current_title);
      current_title = strdup(new_title);
      fprintf(stderr, "Updated title to: %s\n", new_title);
      emit_properties_changed(conn);
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
      append_metadata(&variant);
      dbus_message_iter_close_container(&iter, &variant);
    }
    else if (strcmp(prop_name, "Identity") == 0) {
      dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &variant);
      const char *identity = "Identity";
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &identity);
      dbus_message_iter_close_container(&iter, &variant);
    }
    else if (strcmp(prop_name, "DesktopEntry") == 0) {
      dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &variant);
      const char *desktop = "DesktopEntry";
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &desktop);
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
    
    // Add Identity
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *identity_key = "Identity";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &identity_key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    const char *identity = "Identity";
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &identity);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
    // Add DesktopEntry
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *desktop_key = "DesktopEntry";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &desktop_key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    const char *desktop = "DesktopEntry";
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &desktop);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
    
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
    append_metadata(&variant);
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
  
  current_title = strdup("No track");
  	
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
