#include <glad/glad.h>
#include <client.h>

void overlay_render(Client *client) {
    glDisable(GL_DEPTH_TEST);
    ui_update_size(client->ui);
}
