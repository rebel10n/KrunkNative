#include <client.h>
#include <stdlib.h>

void main_menu_render(MainMenu *ui, int width, int height) {
    vec4 fill_yellow = {1.0f, 1.0f, 0.0f, 1.0f};

    ui_draw_color((UI *) ui, fill_yellow);
    ui_fill_rect((UI *) ui, (int) ((float) width * 0.5f - 5.0f), (int) ((float) height * 0.5f - 1.0f), 10, 2);
    ui_fill_rect((UI *) ui, (int) ((float) width * 0.5f - 1.0f), (int) ((float) height * 0.5f - 5.0f), 2, 10);
}

static UIVTable main_menu_vtable = { (void *) main_menu_render };

MainMenu *main_menu_init() {
    MainMenu *ui = calloc(1, sizeof(MainMenu));
    ui->base.vtable = &main_menu_vtable;

    ui_init((UI *) ui);

    return ui;
}