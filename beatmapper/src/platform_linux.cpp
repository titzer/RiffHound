#include "platform.h"
#include <gtk/gtk.h>
#include <string.h>

// GTK must be initialized before any dialog is shown.
// Calling this multiple times is safe.
static void ensure_gtk_init() {
  static bool initialized = false;
  if (!initialized) {
    int argc = 0;
    gtk_init(&argc, nullptr);
    initialized = true;
  }
}

bool platform_open_file_dialog(char *out_path, int out_size) {
  ensure_gtk_init();

  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      "Open Audio File", nullptr, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Audio Files");
  gtk_file_filter_add_pattern(filter, "*.mp3");
  gtk_file_filter_add_pattern(filter, "*.wav");
  gtk_file_filter_add_pattern(filter, "*.flac");
  gtk_file_filter_add_pattern(filter, "*.m4a");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

  bool result = false;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    strncpy(out_path, filename, out_size - 1);
    out_path[out_size - 1] = '\0';
    g_free(filename);
    result = true;
  }

  gtk_widget_destroy(dialog);
  while (gtk_events_pending())
    gtk_main_iteration();
  return result;
}

bool platform_open_beatmap_dialog(char *out_path, int out_size) {
  ensure_gtk_init();

  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      "Open Beatmap", nullptr, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Text Files");
  gtk_file_filter_add_pattern(filter, "*.txt");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

  bool result = false;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    strncpy(out_path, filename, out_size - 1);
    out_path[out_size - 1] = '\0';
    g_free(filename);
    result = true;
  }

  gtk_widget_destroy(dialog);
  while (gtk_events_pending())
    gtk_main_iteration();
  return result;
}

bool platform_save_beatmap_dialog(char *out_path, int out_size,
                                  const char *suggested_name) {
  ensure_gtk_init();

  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      "Save Beatmap", nullptr, GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);

  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog),
                                                 TRUE);

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Text Files");
  gtk_file_filter_add_pattern(filter, "*.txt");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

  if (suggested_name && suggested_name[0])
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested_name);

  bool result = false;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    strncpy(out_path, filename, out_size - 1);
    out_path[out_size - 1] = '\0';
    g_free(filename);
    result = true;
  }

  gtk_widget_destroy(dialog);
  while (gtk_events_pending())
    gtk_main_iteration();
  return result;
}

void platform_install_fullscreen_shortcut() {
  // On Linux, GLFW handles keyboard input directly and there's no global
  // event monitor like NSEvent. The fullscreen shortcut (e.g. F11 or your
  // chosen binding) should be handled in your GLFW key callback instead.
  // This function intentionally left as a no-op.
}
