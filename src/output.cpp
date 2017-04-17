#include "opengl.hpp"
#include "output.hpp"
#include "signal_definitions.hpp"
#include <linux/input.h>

#include "wm.hpp"
#include "img.hpp"

#include <sstream>
#include <memory>
#include <dlfcn.h>
#include <algorithm>

#include <EGL/egl.h>
#include <EGL/eglext.h>

/* Start plugin manager */
plugin_manager::plugin_manager(wayfire_output *o, wayfire_config *config)
{
    init_default_plugins();
    load_dynamic_plugins();

    for (auto p : plugins) {
        p->grab_interface = new wayfire_grab_interface_t(o);
        p->output = o;

        p->init(config);
    }
}

plugin_manager::~plugin_manager()
{
    for (auto p : plugins) {
        p->fini();
        delete p->grab_interface;

        if (p->dynamic)
            dlclose(p->handle);
        p.reset();
    }
}

namespace
{
template<class A, class B> B union_cast(A object)
{
    union {
        A x;
        B y;
    } helper;
    helper.x = object;
    return helper.y;
}
}

wayfire_plugin plugin_manager::load_plugin_from_file(std::string path, void **h)
{
    void *handle = dlopen(path.c_str(), RTLD_NOW);
    if(handle == NULL) {
        errio << "Can't load plugin " << path << std::endl;
        errio << "\t" << dlerror() << std::endl;
        return nullptr;
    }

    debug << "Loading plugin " << path << std::endl;

    auto initptr = dlsym(handle, "newInstance");
    if(initptr == NULL) {
        errio << "Missing function newInstance in file " << path << std::endl;
        errio << dlerror();
        return nullptr;
    }
    get_plugin_instance_t init = union_cast<void*, get_plugin_instance_t> (initptr);
    *h = handle;
    return wayfire_plugin(init());
}

void plugin_manager::load_dynamic_plugins()
{
    std::stringstream stream(core->plugins);
    auto path = core->plugin_path + "/wayfire/";

    std::string plugin;
    while(stream >> plugin) {
        if(plugin != "") {
            void *handle;
            auto ptr = load_plugin_from_file(path + "/lib" + plugin + ".so", &handle);
            if(ptr) {
                ptr->handle  = handle;
                ptr->dynamic = true;
                plugins.push_back(ptr);
            }
        }
    }
}

template<class T>
wayfire_plugin plugin_manager::create_plugin()
{
    return std::static_pointer_cast<wayfire_plugin_t>(std::make_shared<T>());
}

void plugin_manager::init_default_plugins()
{
    // TODO: rewrite default plugins */
    plugins.push_back(create_plugin<wayfire_focus>());
    /*
    plugins.push_back(create_plugin<Exit>());
    plugins.push_back(create_plugin<Close>());
    plugins.push_back(create_plugin<Refresh>());
    */
}

/* End plugin_manager */

/* Start render_manager */
/* this is a hack, wayland backend has borders set to 38, 38 so honour them */
static int bg_dx = 0, bg_dy = 0;

render_manager::render_manager(wayfire_output *o)
{
    output = o;

    pixman_region32_init(&old_damage);
    pixman_region32_copy(&old_damage, &output->handle->region);

    if (core->backend == WESTON_BACKEND_WAYLAND) {
        debug << "Yes, try it" << std::endl;
        bg_dx = bg_dy = 38;
    }
}

/* TODO: do not rely on glBlitFramebuffer, provide fallback
 * to texture rendering for older systems */
void render_manager::load_background()
{
    background.tex = image_io::load_from_file(core->background, background.w, background.h);

    GL_CALL(glGenFramebuffers(1, &background.fbuff));
    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, background.fbuff));

    GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   background.tex, 0));

    auto status = GL_CALL(glCheckFramebufferStatus(GL_FRAMEBUFFER));
    if (status != GL_FRAMEBUFFER_COMPLETE)
        errio << "Can't setup background framebuffer!" << std::endl;

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

void render_manager::load_context()
{
    ctx = OpenGL::create_gles_context(output, core->shadersrc.c_str());
    OpenGL::bind_context(ctx);
    load_background();

    dirty_context = false;

    output->signal->emit_signal("reload-gl", nullptr);
}

void render_manager::release_context()
{
    GL_CALL(glDeleteFramebuffers(1, &background.fbuff));
    GL_CALL(glDeleteTextures(1, &background.tex));

    OpenGL::release_context(ctx);
    dirty_context = true;
}

#ifdef USE_GLES3
void render_manager::blit_background(GLuint dest, pixman_region32_t *damage)
{
    background.times_blitted++;
    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dest));
    GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, background.fbuff));

    int nrects;
    auto rects = pixman_region32_rectangles(damage, &nrects);
    for (int i = 0; i < nrects; i++) {
        rects[i].x1 -= output->handle->x;
        rects[i].x2 -= output->handle->x;
        rects[i].y1 -= output->handle->y;
        rects[i].y2 -= output->handle->y;

        double topx = rects[i].x1 * 1.0 / output->handle->width;
        double topy = rects[i].y1 * 1.0 / output->handle->height;
        double botx = rects[i].x2 * 1.0 / output->handle->width;
        double boty = rects[i].y2 * 1.0 / output->handle->height;

        GL_CALL(glBlitFramebuffer(topx * background.w, topy * background.h,
                                  botx * background.w, boty * background.h,
                                  bg_dx + rects[i].x1, output->handle->height - rects[i].y1 + bg_dy,
                                  bg_dx + rects[i].x2, output->handle->height - rects[i].y2 + bg_dy, GL_COLOR_BUFFER_BIT, GL_LINEAR));
    }

    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
    GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, 0));
}
#endif

void redraw_idle_cb(void *data)
{
    wayfire_output *output = (wayfire_output*) data;
    assert(output);

    weston_output_schedule_repaint(output->handle);
}

void render_manager::auto_redraw(bool redraw)
{
    if (redraw == constant_redraw) /* no change, exit */
        return;

    constant_redraw = redraw;
    auto loop = wl_display_get_event_loop(core->ec->wl_display);
    wl_event_loop_add_idle(loop, redraw_idle_cb, output);

    if (!constant_redraw) {
        background.times_blitted = 0;

        pixman_region32_fini(&old_damage);
        pixman_region32_init(&old_damage);
        pixman_region32_copy(&old_damage, &output->handle->region);
    }
}

void render_manager::reset_renderer()
{
    renderer = nullptr;
    /* TODO: move to core.cpp */
    //core->ec->renderer->repaint_output = weston_renderer_repaint;
    weston_output_damage(output->handle);
    weston_output_schedule_repaint(output->handle);
}

void render_manager::set_renderer(render_hook_t rh)
{
    if (!rh) {
        renderer = std::bind(std::mem_fn(&render_manager::transformation_renderer), this);
    } else {
        renderer = rh;
    }
}

void render_manager::update_damage(pixman_region32_t *cur_damage, pixman_region32_t *total)
{
    pixman_region32_init(total);
    pixman_region32_union(total, cur_damage, &old_damage);
    pixman_region32_copy(&old_damage, cur_damage);
}

struct weston_gl_renderer {
    weston_renderer base;
    int a, b;
    void *c, *d;
    EGLDisplay display;
    EGLContext context;
};

void initial_background_render_idle_cb(void *data)
{
    auto output = (wayfire_output*) data;
    weston_output_schedule_repaint(output->handle);
}
void render_manager::paint(pixman_region32_t *damage)
{
    pixman_region32_t total_damage;

    if (dirty_context) {
        load_context();
        core->weston_repaint(output->handle, damage);

        auto loop = wl_display_get_event_loop(core->ec->wl_display);
        wl_event_loop_add_idle(loop, redraw_idle_cb, output);
        return;
    }
    // This is a hack, weston renderer_state is a struct and the EGLSurface is the first field
    // In the future this might change so we need to track changes in weston
    EGLSurface surf = *(EGLSurface*)output->handle->renderer_state;
    weston_gl_renderer *gr = (weston_gl_renderer*) core->ec->renderer;
    eglMakeCurrent(gr->display, surf, surf, gr->context);

    GL_CALL(glViewport(output->handle->x, output->handle->y,
                output->handle->width, output->handle->height));

    /* if we are on the second frame, we haven't fully drawn the background
     * to the second buffer(which is now back buffer), so just blit the whole */
    if (background.times_blitted == 1) {
        pixman_region32_fini(damage);
        pixman_region32_init(damage);
        pixman_region32_copy(damage, &output->handle->region);
    }

    if (renderer) {
        OpenGL::bind_context(ctx);
        renderer();

        wl_signal_emit(&output->handle->frame_signal, output->handle);
        eglSwapBuffers(gr->display, surf);
    } else {
        update_damage(damage, &total_damage);
        blit_background(0, &total_damage);
        core->weston_repaint(output->handle, damage);
    }

    if (constant_redraw) {
        wl_event_loop_add_idle(wl_display_get_event_loop(core->ec->wl_display),
                redraw_idle_cb, output);
    }
}

void render_manager::pre_paint()
{
    std::vector<effect_hook_t*> active_effects;
    for (auto effect : output_effects) {
        active_effects.push_back(effect);
    }

    for (auto& effect : active_effects)
        (*effect)();
}

void render_manager::transformation_renderer()
{
    blit_background(0, &output->handle->region);
    output->workspace->for_each_view_reverse([=](wayfire_view v) {
        if (!v->destroyed && !v->is_hidden)
            v->render();
    });
}

static int effect_hook_last_id = 0;
void render_manager::add_output_effect(effect_hook_t* hook, wayfire_view v)
{
    if (v)
        v->effects.push_back(hook);
    else
        output_effects.push_back(hook);
}

void render_manager::rem_effect(const effect_hook_t *hook, wayfire_view v)
{
    decltype(output_effects)& container = output_effects;
    if (v) container = v->effects;
    auto it = std::remove_if(container.begin(), container.end(),
    [hook] (const effect_hook_t *h) {
        if (h == hook)
            return true;
        return false;
    });

    container.erase(it, container.end());
}
/* End render_manager */

/* Start SignalManager */

void signal_manager::connect_signal(std::string name, signal_callback_t* callback)
{
    sig[name].push_back(callback);
}

void signal_manager::disconnect_signal(std::string name, signal_callback_t* callback)
{
    auto it = std::remove_if(sig[name].begin(), sig[name].end(),
    [=] (const signal_callback_t *call) {
        return call == callback;
    });

    sig[name].erase(it, sig[name].end());
}

void signal_manager::emit_signal(std::string name, signal_data *data)
{
    std::vector<signal_callback_t> callbacks;
    for (auto x : sig[name])
        callbacks.push_back(*x);

    for (auto x : callbacks)
        x(data);
}

/* End SignalManager */
/* Start output */
wayfire_output* wl_output_to_wayfire_output(uint32_t output)
{
    wayfire_output *result = nullptr;
    core->for_each_output([output, &result] (wayfire_output *wo) {
        if (wo->handle->id == output)
            result = wo;
    });

    return result;
}

void shell_add_background(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface, int32_t x, int32_t y)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    auto wo = wl_output_to_wayfire_output(output);

    wayfire_view view = nullptr;

    if (!wo || !wsurf || !(view = core->find_view(wsurf))) {
        errio << "shell_add_background called with invalid surface or output" << std::endl;
        return;
    }

    debug << "wf_shell: add_background" << std::endl;
    wo->workspace->add_background(view, x, y);
}

void shell_add_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    auto wo = wl_output_to_wayfire_output(output);

    wayfire_view view = nullptr;

    if (!wo || !wsurf || !(view = core->find_view(wsurf))) {
        errio << "shell_add_panel called with invalid surface or output" << std::endl;
        return;
    }

    debug << "wf_shell: add_panel" << std::endl;
    wo->workspace->add_panel(view);
}

void shell_configure_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface, int32_t x, int32_t y)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    auto wo = wl_output_to_wayfire_output(output);

    wayfire_view view = nullptr;

    if (!wo || !wsurf || !(view = core->find_view(wsurf))) {
        errio << "shell_configure_panel called with invalid surface or output" << std::endl;
        return;
    }

    debug << "wf_shell: configure_panel" << std::endl;
    wo->workspace->configure_panel(view, x, y);
}

void shell_reserve(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, uint32_t side, uint32_t width, uint32_t height)
{
    auto wo = wl_output_to_wayfire_output(output);

    if (!wo) {
        errio << "shell_reserve called with invalid output" << std::endl;
        return;
    }

    debug << "wf_shell: reserve" << std::endl;
    wo->workspace->reserve_workarea((wayfire_shell_panel_position)side, width, height);
}

const struct wayfire_shell_interface shell_interface_impl {
    .add_background = shell_add_background,
    .add_panel = shell_add_panel,
    .configure_panel = shell_configure_panel,
    .reserve = shell_reserve
};

wayfire_output::wayfire_output(weston_output *handle, wayfire_config *c)
{
    this->handle = handle;

    render = new render_manager(this);
    signal = new signal_manager();

    plugin = new plugin_manager(this, c);
    weston_output_damage(handle);
    weston_output_schedule_repaint(handle);
}

wayfire_output::~wayfire_output()
{
    delete plugin;
    delete signal;
    delete render;
}

wayfire_geometry wayfire_output::get_full_geometry()
{
    return {.origin = {handle->x, handle->y},
            .size = {handle->width, handle->height}};
}

void wayfire_output::activate()
{
}

void wayfire_output::deactivate()
{
    // TODO: what do we do?
    //render->dirty_context = true;
}

void wayfire_output::attach_view(wayfire_view v)
{
    v->output = this;

    workspace->view_bring_to_front(v);
    auto sig_data = create_view_signal{v};
    signal->emit_signal("create-view", &sig_data);
}

void wayfire_output::detach_view(wayfire_view v)
{
    workspace->view_removed(v);

    wayfire_view next = nullptr;

    auto views = workspace->get_views_on_workspace(workspace->get_current_workspace());
    for (auto wview : views) {
        if (wview->handle != v->handle && wview->is_mapped) {
            next = wview;
            break;
        }
    }

    if (active_view == v) {
        if (next == nullptr) {
            active_view = nullptr;
        } else {
            focus_view(next, core->get_current_seat());
        }
    }

    auto sig_data = destroy_view_signal{v};
    signal->emit_signal("destroy-view", &sig_data);
}

void wayfire_output::bring_to_front(wayfire_view v) {
    assert(v);

    weston_view_geometry_dirty(v->handle);
    weston_layer_entry_remove(&v->handle->layer_link);

    workspace->view_bring_to_front(v);

    weston_view_geometry_dirty(v->handle);
    weston_surface_damage(v->surface);
    weston_desktop_surface_propagate_layer(v->desktop_surface);
}

void wayfire_output::focus_view(wayfire_view v, weston_seat *seat)
{
    if (v == active_view)
        return;

    if (active_view && !active_view->destroyed)
        weston_desktop_surface_set_activated(active_view->desktop_surface, false);

    active_view = v;
    if (active_view) {
        debug << "output: " << handle->id << " focus: " << v->desktop_surface << std::endl;
        weston_view_activate(v->handle, seat,
                             WESTON_ACTIVATE_FLAG_CLICKED | WESTON_ACTIVATE_FLAG_CONFIGURE);
        weston_desktop_surface_set_activated(v->desktop_surface, true);
        bring_to_front(v);
    } else {
        debug << "output: " << handle->id << " focus: 0" << std::endl;
        weston_keyboard_set_focus(weston_seat_get_keyboard(seat), NULL);
    }
}

wayfire_view wayfire_output::get_top_view()
{
    if (active_view)
        return active_view;

    wayfire_view view;
    workspace->for_each_view([&view] (wayfire_view v) {
        if (!view)
            view = v;
    });

    return view;
}

wayfire_view wayfire_output::get_view_at_point(int x, int y)
{
    wayfire_view chosen = nullptr;

    workspace->for_each_view([x, y, &chosen] (wayfire_view v) {
        if (v->is_visible() && point_inside({x, y}, v->geometry)) {
            if (chosen == nullptr)
                chosen = v;
        }
    });

    return chosen;
}

bool wayfire_output::activate_plugin(wayfire_grab_interface owner)
{
    if (!owner)
        return false;

    if (core->get_active_output() != this)
        return false;

    if (active_plugins.find(owner) != active_plugins.end())
        return true;

    for(auto act_owner : active_plugins) {
        bool owner_in_act_owner_compat =
            act_owner->compat.find(owner->name) != act_owner->compat.end();

        bool act_owner_in_owner_compat =
            owner->compat.find(act_owner->name) != owner->compat.end();

        if(!owner_in_act_owner_compat && !act_owner->compatAll)
            return false;

        if(!act_owner_in_owner_compat && !owner->compatAll)
            return false;
    }

    active_plugins.insert(owner);
    return true;
}

bool wayfire_output::deactivate_plugin(wayfire_grab_interface owner)
{
    owner->ungrab();
    active_plugins.erase(owner);
    return true;
}

bool wayfire_output::is_plugin_active(owner_t name)
{
    for (auto act : active_plugins)
        if (act && act->name == name)
            return true;

    return false;
}