#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/dstr.h>
#include <util/base.h>
#include "custom-delay.h"
#include "easing.h"
#include <stdio.h>

struct frame {
	gs_texrender_t *render;
	uint64_t ts;
};

struct custom_delay_info {
	obs_source_t *source;
	struct circlebuf frames;
	obs_hotkey_id skip_begin_hotkey;
	obs_hotkey_id skip_end_hotkey;

	bool hotkeys_loaded;
	double max_duration;
	double speed;
	double start_speed;
	double target_speed;

	double slow_forward_speed;
	double fast_forward_speed;
	double slow_backward_speed;
	double fast_backward_speed;

	uint32_t cx;
	uint32_t cy;
	bool processed_frame;
	double time_diff;
	bool target_valid;

	uint32_t easing;
	float easing_duration;
	float easing_max_duration;
	uint64_t easing_started;

	char *text_source_name;
	char *text_format;
};

static void replace_text(struct dstr *str, size_t pos, size_t len,
			 const char *new_text)
{
	struct dstr front = {0};
	struct dstr back = {0};

	dstr_left(&front, str, pos);
	dstr_right(&back, str, pos + len);
	dstr_copy_dstr(str, &front);
	dstr_cat(str, new_text);
	dstr_cat_dstr(str, &back);
	dstr_free(&front);
	dstr_free(&back);
}

static void custom_delay_text(struct custom_delay_info *c)
{
	if (!c->text_source_name || !strlen(c->text_source_name) ||
	    !c->text_format)
		return;
	obs_source_t *s = obs_get_source_by_name(c->text_source_name);
	if (!s)
		return;

	struct dstr sf;
	size_t pos = 0;
	struct dstr buffer;
	dstr_init(&buffer);
	dstr_init_copy(&sf, c->text_format);
	while (pos < sf.len) {
		const char *cmp = sf.array + pos;
		if (astrcmp_n(cmp, "%SPEED%", 7) == 0) {
			dstr_printf(&buffer, "%.1f", c->speed * 100.0);
			dstr_cat_ch(&buffer, '%');
			replace_text(&sf, pos, 7, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%TARGET%", 8) == 0) {
			dstr_printf(&buffer, "%.1f", c->target_speed * 100.0);
			dstr_cat_ch(&buffer, '%');
			replace_text(&sf, pos, 8, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%TIME%", 6) == 0) {
			dstr_printf(&buffer, "%.1f", c->time_diff);
			replace_text(&sf, pos, 6, buffer.array);
			pos += buffer.len;
		} else {
			pos++;
		}
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", sf.array);
	obs_source_update(s, settings);
	obs_data_release(settings);
	dstr_free(&sf);
	dstr_free(&buffer);
	obs_source_release(s);
}

static const char *custom_delay_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("CustomDelay");
}

static void free_textures(struct custom_delay_info *f)
{

	while (f->frames.size) {
		struct frame frame;
		circlebuf_pop_front(&f->frames, &frame, sizeof(frame));
		obs_enter_graphics();
		gs_texrender_destroy(frame.render);
		obs_leave_graphics();
	}
	circlebuf_free(&f->frames);
}

static void custom_delay_update(void *data, obs_data_t *settings)
{
	struct custom_delay_info *d = data;
	double duration = obs_data_get_double(settings, S_DURATION);
	if (duration < d->max_duration) {
		free_textures(d);
	}
	d->max_duration = duration;
	d->easing = obs_data_get_int(settings, S_EASING);
	d->easing_max_duration =
		(float)obs_data_get_double(settings, S_EASING_DURATION);
	d->slow_forward_speed =
		obs_data_get_double(settings, S_SLOW_FORWARD) / 100.0;
	d->fast_forward_speed =
		obs_data_get_double(settings, S_FAST_FORWARD) / 100.0;
	d->slow_backward_speed =
		obs_data_get_double(settings, S_SLOW_BACKWARD) / 100.0;
	d->fast_backward_speed =
		obs_data_get_double(settings, S_FAST_BACKWARD) / 100.0;

	const char *text_source = obs_data_get_string(settings, S_TEXT_SOURCE);
	if (d->text_source_name) {
		if (strcmp(d->text_source_name, text_source) != 0) {
			bfree(d->text_source_name);
			d->text_source_name = bstrdup(text_source);
		}
	} else {
		d->text_source_name = bstrdup(text_source);
	}

	const char *text_format = obs_data_get_string(settings, S_TEXT_FORMAT);
	if (d->text_format) {
		if (strcmp(d->text_format, text_format) != 0) {
			bfree(d->text_format);
			d->text_format = bstrdup(text_format);
		}
	} else {
		d->text_format = bstrdup(text_format);
	}
}

static void *custom_delay_create(obs_data_t *settings, obs_source_t *source)
{
	struct custom_delay_info *d =
		bzalloc(sizeof(struct custom_delay_info));
	d->source = source;
	d->speed = 1.0;
	d->target_speed = 1.0;
	d->start_speed = 1.0;
	custom_delay_update(d, settings);
	return d;
}

static void custom_delay_destroy(void *data)
{
	struct custom_delay_info *c = data;
	obs_hotkey_unregister(c->skip_begin_hotkey);
	obs_hotkey_unregister(c->skip_end_hotkey);
	free_textures(c);
	if (c->text_source_name)
		bfree(c->text_source_name);
	if (c->text_format)
		bfree(c->text_format);
	bfree(c);
}

void custom_delay_skip_begin_hotkey(void *data, obs_hotkey_id id,
				     obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return;
	struct custom_delay_info *d = data;
	if (d->start_speed < 1.0 || d->speed < 1.0) {
		d->start_speed = 1.0;
		d->target_speed = 1.0;
		d->easing_started = 0;
	}
	d->time_diff = d->max_duration;
}

void custom_delay_skip_end_hotkey(void *data, obs_hotkey_id id,
				   obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return;
	struct custom_delay_info *d = data;
	if (d->start_speed > 1.0 || d->speed > 1.0) {
		d->start_speed = 1.0;
		d->target_speed = 1.0;
		d->easing_started = 0;
	}
	d->time_diff = 0;
}

static void custom_delay_load_hotkeys(void *data)
{
	struct custom_delay_info *d = data;
	obs_source_t *parent = obs_filter_get_parent(d->source);
	if (parent) {
		d->skip_begin_hotkey = obs_hotkey_register_source(
			parent, "skip_begin", obs_module_text("SkipBegin"),
			custom_delay_skip_begin_hotkey, data);
		d->skip_end_hotkey = obs_hotkey_register_source(
			parent, "skip_end", obs_module_text("SkipEnd"),
			custom_delay_skip_end_hotkey, data);
		d->hotkeys_loaded = true;
	}
}

static void custom_delay_load(void *data, obs_data_t *settings)
{
	custom_delay_load_hotkeys(data);
	custom_delay_update(data, settings);
}

static void draw_frame(struct custom_delay_info *d)
{
	struct frame *frame = NULL;
	if (!d->frames.size)
		return;
	const size_t count = d->frames.size / sizeof(struct frame);
	if (d->time_diff <= 0.0) {
		frame = circlebuf_data(&d->frames,
				       (count - 1) * sizeof(struct frame));
	} else {
		size_t i = 0;
		const uint64_t ts = obs_get_video_frame_time();
		while (i < count) {
			frame = circlebuf_data(&d->frames,
					       i * sizeof(struct frame));
			if (ts - frame->ts <
			    (uint64_t)(d->time_diff * 1000000000.0))
				break;
			i++;
		}
	}
	if (!frame)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(frame->render);
	if (tex) {
		gs_eparam_t *image =
			gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, tex);

		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, d->cx, d->cy);
	}
}

static void custom_delay_video_render(void *data, gs_effect_t *effect)
{
	struct custom_delay_info *d = data;
	obs_source_t *target = obs_filter_get_target(d->source);
	obs_source_t *parent = obs_filter_get_parent(d->source);

	if (!d->target_valid || !target || !parent) {
		obs_source_skip_video_filter(d->source);
		return;
	}
	if (d->processed_frame) {
		draw_frame(d);
		return;
	}

	const uint64_t ts = obs_get_video_frame_time();
	struct frame frame;
	frame.render = NULL;
	if (d->frames.size) {
		circlebuf_peek_front(&d->frames, &frame, sizeof(frame));
		if (ts > frame.ts &&
		    ts - frame.ts <
			    (uint64_t)(d->max_duration * 1000000000.0)) {
			frame.render = NULL;
		} else {
			circlebuf_pop_front(&d->frames, NULL, sizeof(frame));
			if (d->frames.size) {
				struct frame next_frame;
				circlebuf_peek_front(&d->frames, &next_frame,
						     sizeof(next_frame));
				if ((ts > next_frame.ts ? ts - next_frame.ts
							: next_frame.ts - ts) >=
				    (uint64_t)(d->max_duration *
					       1000000000.0)) {
					gs_texrender_destroy(frame.render);
					circlebuf_pop_front(&d->frames, &frame,
							    sizeof(frame));
				}
			}
		}
	}
	if (!frame.render) {
		frame.render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	} else {
		gs_texrender_reset(frame.render);
	}
	frame.ts = ts;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(frame.render, d->cx, d->cy)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)d->cx, 0.0f, (float)d->cy, -100.0f,
			 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(frame.render);
	}

	gs_blend_state_pop();

	circlebuf_push_back(&d->frames, &frame, sizeof(frame));

	draw_frame(d);
	d->processed_frame = true;
}

static bool custom_delay_text_source_modified(obs_properties_t *props,
					       obs_property_t *property,
					       obs_data_t *data)
{
	const char *source_name = obs_data_get_string(data, S_TEXT_SOURCE);
	bool text_source = false;
	if (source_name && strlen(source_name)) {
		text_source = true;
	}
	obs_property_t *prop = obs_properties_get(props, S_TEXT_FORMAT);
	obs_property_set_visible(prop, text_source);
	return true;
}

static obs_properties_t *custom_delay_properties(void *data)
{
	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p = obs_properties_add_float(
		ppts, S_DURATION, obs_module_text("Duration"), 0.0, 10000.0,
		1.0);
	obs_property_float_set_suffix(p, "s");
	return ppts;
}

void custom_delay_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_DURATION, 5.0);
}

static inline void check_size(struct custom_delay_info *d)
{
	obs_source_t *target = obs_filter_get_target(d->source);

	d->target_valid = !!target;
	if (!d->target_valid)
		return;

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);

	d->target_valid = !!cx && !!cy;
	if (!d->target_valid)
		return;

	if (cx != d->cx || cy != d->cy) {
		d->cx = cx;
		d->cy = cy;
		free_textures(d);
	}
}

static void custom_delay_tick(void *data, float t)
{
	struct custom_delay_info *d = data;
	if (!d->hotkeys_loaded)
		custom_delay_load_hotkeys(data);
	d->processed_frame = false;
	if (d->speed != d->target_speed) {
		const uint64_t ts = obs_get_video_frame_time();
		if (!d->easing_started)
			d->easing_started = ts;
		const double duration =
			(double)(ts - d->easing_started) / 1000000000.0;
		if (duration > d->easing_max_duration ||
		    d->easing_max_duration <= 0.0) {
			d->speed = d->target_speed;
		} else {
			double t2 = duration / d->easing_max_duration;
			if (d->easing == EASING_QUADRATIC) {
				t2 = QuadraticEaseInOut(t2);
			} else if (d->easing == EASING_CUBIC) {
				t2 = CubicEaseInOut(t2);
			} else if (d->easing == EASING_QUARTIC) {
				t2 = QuarticEaseInOut(t2);
			} else if (d->easing == EASING_QUINTIC) {
				t2 = QuinticEaseInOut(t2);
			} else if (d->easing == EASING_SINE) {
				t2 = SineEaseInOut(t2);
			} else if (d->easing == EASING_CIRCULAR) {
				t2 = CircularEaseInOut(t2);
			} else if (d->easing == EASING_EXPONENTIAL) {
				t2 = ExponentialEaseInOut(t2);
			} else if (d->easing == EASING_ELASTIC) {
				t2 = ElasticEaseInOut(t2);
			} else if (d->easing == EASING_BOUNCE) {
				t2 = BounceEaseInOut(t2);
			} else if (d->easing == EASING_BACK) {
				t2 = BackEaseInOut(t2);
			}
			d->speed = d->start_speed +
				   (d->target_speed - d->start_speed) * t2;
		}
	} else if (d->easing_started) {
		d->easing_started = 0;
	}
	if (d->speed > 1.0 && d->target_speed > 1.0 &&
	    d->time_diff < d->easing_max_duration / 2.0) {
		d->start_speed = d->speed;
		d->target_speed = 1.0;
		d->easing_started = 0;
	} else if (d->speed < 1.0 && d->target_speed < 1.0 &&
		   d->max_duration - d->time_diff <
			   d->easing_max_duration / 2.0) {
		d->start_speed = d->speed;
		d->target_speed = 1.0;
		d->easing_started = 0;
	}
	double time_diff = d->time_diff;
	time_diff += (1.0 - d->speed) * t;
	if (time_diff < 0.0)
		time_diff = 0.0;
	if (time_diff > d->max_duration)
		time_diff = d->max_duration;
	d->time_diff = time_diff;
	custom_delay_text(d);
	check_size(d);
}

void custom_delay_activate(void *data)
{
	struct custom_delay_info *d = data;
}

void custom_delay_deactivate(void *data)
{
	struct custom_delay_info *d = data;
}

void custom_delay_show(void *data)
{
	struct custom_delay_info *d = data;
}

void custom_delay_hide(void *data)
{
	struct custom_delay_info *d = data;
}

struct obs_source_info custom_delay_filter = {
	.id = "custom_delay_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_OUTPUT_VIDEO,
	.get_name = custom_delay_get_name,
	.create = custom_delay_create,
	.destroy = custom_delay_destroy,
	.load = custom_delay_load,
	.update = custom_delay_update,
	.video_render = custom_delay_video_render,
	.get_properties = custom_delay_properties,
	.get_defaults = custom_delay_defaults,
	.video_tick = custom_delay_tick,
	.activate = custom_delay_activate,
	.deactivate = custom_delay_deactivate,
	.show = custom_delay_show,
	.hide = custom_delay_hide,
};

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Metabig");
OBS_MODULE_USE_DEFAULT_LOCALE("custom-delay", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("customDelay");
}

bool obs_module_load(void)
{
	obs_register_source(&custom_delay_filter);
	return true;
}
