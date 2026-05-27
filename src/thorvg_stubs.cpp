extern "C" {
void tvg_engine_init(unsigned) {}
void tvg_engine_term() {}
void* tvg_swcanvas_create() { return nullptr; }
void tvg_swcanvas_set_target(void*,void*,void*,unsigned,unsigned,unsigned,unsigned,unsigned) {}
void tvg_canvas_set_viewport(void*,int,int,int,int) {}
void tvg_canvas_draw(void*) {}
void tvg_canvas_destroy(void*) {}
void tvg_canvas_sync(void*) {}
void tvg_canvas_push(void*,void*) {}
void tvg_animation_del(void*) {}
void* tvg_animation_new() { return nullptr; }
void* tvg_animation_get_picture(void*) { return nullptr; }
void tvg_animation_set_frame(void*,float) {}
void tvg_gradient_set_color_stops(void*,void*,int) {}
void tvg_gradient_set_spread(void*,int) {}
void tvg_gradient_set_transform(void*,void*) {}
void* tvg_shape_new() { return nullptr; }
void tvg_shape_cubic_to(void*,float,float,float,float,float,float) {}
void tvg_shape_set_fill_rule(void*,int) {}
void tvg_shape_set_stroke_color(void*,void*) {}
void tvg_shape_set_stroke_width(void*,float) {}
void tvg_shape_set_stroke_miterlimit(void*,float) {}
void tvg_shape_set_stroke_cap(void*,int) {}
void tvg_shape_set_stroke_join(void*,int) {}
void tvg_paint_set_blend_method(void*,int) {}
void tvg_paint_set_opacity(void*,unsigned char) {}
void tvg_shape_move_to(void*,float,float) {}
void tvg_shape_line_to(void*,float,float) {}
void tvg_shape_close(void*) {}
void tvg_shape_set_fill_color(void*,void*) {}
void* tvg_linear_gradient_new() { return nullptr; }
void tvg_linear_gradient_set(void*,float,float,float,float) {}
void tvg_shape_set_stroke_linear_gradient(void*,void*) {}
void tvg_paint_set_transform(void*,void*) {}
void tvg_shape_append_rect(void*,float,float,float,float,float,float) {}
void tvg_shape_set_stroke_dash(void*,void*,int) {}
void* tvg_picture_new() { return nullptr; }
void tvg_picture_load_raw(void*,void*,unsigned,unsigned,unsigned char) {}
void* tvg_paint_duplicate(void*) { return nullptr; }
void tvg_paint_set_composite_method(void*,int) {}
void* tvg_radial_gradient_new() { return nullptr; }
void tvg_radial_gradient_set(void*,float,float,float) {}
void tvg_shape_set_radial_gradient(void*,void*) {}
void tvg_shape_set_linear_gradient(void*,void*) {}
void tvg_paint_get_bounds(void*,float*,float*,float*,float*) {}
void tvg_shape_set_stroke_radial_gradient(void*,void*) {}
void tvg_canvas_update(void*) {}
void tvg_picture_set_size(void*,float,float) {}
void tvg_animation_get_frame(void*) {}
void tvg_picture_load_data(void*,void*,unsigned,const char*,unsigned char) {}
void tvg_animation_get_total_frame(void*) {}
void tvg_picture_load(void*,const char*) {}
}
