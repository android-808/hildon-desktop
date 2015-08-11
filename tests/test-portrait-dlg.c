#include <stdlib.h>
#include <hildon/hildon.h>

#include "portrait-common.c"

int main(int argc, char const *argv[])
{
  GtkWidget *dlg;

  gtk_init(NULL, NULL);
  GtkDialogFlags flags = GTK_DIALOG_MODAL;
  dlg = gtk_dialog_new_with_buttons ("Port. Dlg Test",
                                      NULL,
                                      flags,
                                      "OK",
                                      GTK_RESPONSE_ACCEPT,
                                      "Cancel",
                                      GTK_RESPONSE_REJECT,
                                      NULL);

  init_portrait(GTK_WIDGET(dlg), argv);

  gtk_window_set_title(GTK_WINDOW(dlg), "Portrait dialog");
  gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area (GTK_DIALOG(dlg))), portrait());
  gtk_widget_show_all(dlg);
  gtk_dialog_run(GTK_DIALOG(dlg));

  return 0;
}
