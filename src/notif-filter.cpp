#include "notif-filter.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kMaxChars = 5000;
constexpr float kPi = 3.14159265358979f;

enum class State { Idle, In, Hold, Out };

// Todos los ajustes que llegan desde el dock.
struct Config {
	std::string text = "Escribe aquí tu frase";
	int direction = 0; // 0 = desde arriba, 1 = desde la izquierda, 2 = desde la derecha
	float anim_in_sec = 1.0f;
float anim_out_sec = 1.0f;
	float hold_sec = 5.0f; // segundos visible (solo en modo por tiempo)
	int mode = 0;          // 0 = funcion por tiempo, 1 = funcion por accion
	float blur_max = 30.0f;
	std::string font_family = "Arial";
	std::string font_file;
	int font_size = 42;
	bool bold = true;
	uint32_t bg = 0xFFFF4010;   // ABGR de OBS: azul
	uint32_t fg = 0xFFFFFFFF;   // blanco
	bool shadow = false;
	int shadow_angle = 90;      // 0 = derecha, 90 = abajo (sentido horario)
	int shadow_len = 16;
	int shadow_blur = 24;       // difuminado de la sombra, en pixeles
	int shadow_amount = 50;     // porcentaje
};

struct NotifFilter {
	obs_source_t *source = nullptr;

	std::mutex mtx;
	std::shared_ptr<const Config> cfg;
	uint64_t cfg_version = 0;

	std::atomic<bool> want_show{false};
	std::atomic<bool> want_hide{false};
	std::atomic<bool> want_toggle{false};
	std::atomic<bool> visible{false};

	State state = State::Idle;
	float prog = 0.0f;
	float hold_t = 0.0f;

	gs_effect_t *effect = nullptr;
	gs_texrender_t *rt = nullptr;
	gs_texrender_t *rt2 = nullptr;

	gs_texture_t *bubble = nullptr;
	int bub_w = 0, bub_h = 0, bub_pad = 0, bub_margin = 0;
	std::shared_ptr<const Config> bub_cfg;
	uint32_t bub_cw = 0, bub_ch = 0;

	// Transicion de tamaño cuando el texto cambia con la notificacion ya visible.
	int old_w = 0, old_h = 0;   // tamaño de la burbuja anterior
	float morph = 1.0f;         // 0 = tamaño anterior, 1 = tamaño nuevo (terminada)
	bool bub_drawn = false;     // la burbuja actual ya se mostro en pantalla
	gs_texture_t *old_tex = nullptr;     // burbuja anterior, solo durante el fundido
	gs_effect_t *fade_effect = nullptr;  // dibuja una textura con opacidad ajustable
};

// Efecto propio: igual al de dibujo normal pero con un parametro de opacidad.
static const char *kFadeEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float alpha = 1.0;

sampler_state def_sampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 PSFade(VertData v_in) : TARGET
{
	float4 c = image.Sample(def_sampler, v_in.uv);
	c.a *= alpha;
	return c;
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSFade(v_in);
	}
}
)";

QColor to_qcolor(uint32_t c)
{
	return QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF), int((c >> 24) & 0xFF));
}

void register_font_file(const std::string &path)
{
	static std::mutex m;
	static std::set<std::string> done;
	if (path.empty())
		return;
	std::lock_guard<std::mutex> lock(m);
	if (done.insert(path).second)
		QFontDatabase::addApplicationFont(QString::fromUtf8(path.c_str()));
}

// Arma los ajustes a partir de la configuración general del plugin (igual para todas las fuentes).
std::shared_ptr<const Config> build_config()
{
	auto c = std::make_shared<Config>();

	notif_config_read([&](obs_data_t *s) {
		c->text = QString::fromUtf8(obs_data_get_string(s, "text")).left(kMaxChars).toUtf8().toStdString();
		c->direction = int(obs_data_get_int(s, "direction"));
		c->anim_in_sec = std::max(0.01f, float(obs_data_get_double(s, "anim_in_sec")));
c->anim_out_sec = std::max(0.01f, float(obs_data_get_double(s, "anim_out_sec")));
		c->hold_sec = std::max(0.0f, float(obs_data_get_double(s, "hold_sec")));
		c->mode = int(obs_data_get_int(s, "mode"));
		c->blur_max = float(obs_data_get_double(s, "blur_max"));
		c->font_family = obs_data_get_string(s, "font_family");
		c->font_file = obs_data_get_string(s, "font_file");
		c->font_size = int(obs_data_get_int(s, "font_size"));
		c->bold = obs_data_get_bool(s, "bold");
		c->bg = uint32_t(obs_data_get_int(s, "bg_color"));
		c->fg = uint32_t(obs_data_get_int(s, "text_color"));
		c->shadow = obs_data_get_bool(s, "shadow");
		c->shadow_angle = int(obs_data_get_int(s, "shadow_angle"));
		c->shadow_len = int(obs_data_get_int(s, "shadow_len"));
		c->shadow_blur = int(obs_data_get_int(s, "shadow_blur"));
		c->shadow_amount = int(obs_data_get_int(s, "shadow_amount"));
	});

	return c;
}

// Cada fuente vuelve a armar sus ajustes solo cuando la configuración general cambió.
std::shared_ptr<const Config> get_cfg(NotifFilter *f)
{
	const uint64_t v = notif_config_version();
	std::lock_guard<std::mutex> lock(f->mtx);
	if (!f->cfg || f->cfg_version != v) {
		f->cfg = build_config();
		f->cfg_version = v;
	}
	return f->cfg;
}

// ---------------------------------------------------------------------------
// Desenfoque para la sombra: tres pasadas de caja aproximan un desenfoque gaussiano
// ---------------------------------------------------------------------------
void box_blur(std::vector<float> &a, std::vector<float> &t, int w, int h, int r)
{
	const float inv = 1.0f / float(2 * r + 1);

	// horizontal: a pasa a t
	for (int y = 0; y < h; y++) {
		const float *src = &a[size_t(y) * w];
		float *dst = &t[size_t(y) * w];
		float sum = 0.0f;
		for (int x = 0; x <= r && x < w; x++)
			sum += src[x];
		for (int x = 0; x < w; x++) {
			dst[x] = sum * inv;
			const int add = x + r + 1;
			const int sub = x - r;
			if (add < w)
				sum += src[add];
			if (sub >= 0)
				sum -= src[sub];
		}
	}

	// vertical: t pasa a a
	for (int x = 0; x < w; x++) {
		float sum = 0.0f;
		for (int y = 0; y <= r && y < h; y++)
			sum += t[size_t(y) * w + x];
		for (int y = 0; y < h; y++) {
			a[size_t(y) * w + x] = sum * inv;
			const int add = y + r + 1;
			const int sub = y - r;
			if (add < h)
				sum += t[size_t(add) * w + x];
			if (sub >= 0)
				sum -= t[size_t(sub) * w + x];
		}
	}
}

// Desenfoca solo el canal alfa de una imagen (la sombra es negra, así que alcanza con eso).
void blur_alpha(QImage &img, int radius)
{
	const int w = img.width();
	const int h = img.height();
	std::vector<float> a(size_t(w) * h), t(size_t(w) * h);

	for (int y = 0; y < h; y++) {
		const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
		for (int x = 0; x < w; x++)
			a[size_t(y) * w + x] = float(qAlpha(row[x]));
	}

	for (int i = 0; i < 3; i++)
		box_blur(a, t, w, h, radius);

	for (int y = 0; y < h; y++) {
		QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
		for (int x = 0; x < w; x++) {
			const int v = std::clamp(int(a[size_t(y) * w + x] + 0.5f), 0, 255);
			row[x] = qRgba(0, 0, 0, v);
		}
	}
}

// ---------------------------------------------------------------------------
// Dibujo de la notificación en una imagen (QPainter) que luego pasa a textura
// ---------------------------------------------------------------------------
void render_bubble_image(const Config &c, uint32_t cw, uint32_t ch, QImage &out, int &pad_out, int &margin_out)
{
	const int margin = std::max(12, int(cw * 0.02));
	const int bw = std::max(80, int(cw) - 2 * margin);

	QString text = QString::fromUtf8(c.text.c_str()).left(kMaxChars);
	if (text.isEmpty())
		text = QStringLiteral(" ");

	QFont font(QString::fromUtf8(c.font_family.c_str()));
	font.setBold(c.bold);

	// Si el texto no entra en el alto del lienzo, se achica la letra hasta que entre.
	int fs = std::max(8, c.font_size);
	int padx = 0, pady = 0, textw = 0, texth = 0, bh = 0;
	for (;;) {
		font.setPixelSize(fs);
		padx = int(fs * 0.9);
		pady = int(fs * 0.6);
		textw = std::max(20, bw - 2 * padx);
		QFontMetrics fm(font);
		QRect r = fm.boundingRect(QRect(0, 0, textw, 100000), Qt::TextWordWrap | Qt::AlignLeft, text);
		texth = r.height();
		bh = texth + 2 * pady;
		if (bh <= int(ch) - 2 * margin || fs <= 8)
			break;
		fs = std::max(8, fs - std::max(1, fs / 20));
	}

	int pad = 0;
	float dx = 0.0f, dy = 0.0f;
	if (c.shadow) {
		const float a = c.shadow_angle * kPi / 180.0f;
		dx = std::cos(a) * c.shadow_len;
		dy = std::sin(a) * c.shadow_len;
		pad = c.shadow_len + std::max(0, c.shadow_blur) + 2;
	}

	QImage img(bw + 2 * pad, bh + 2 * pad, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);

	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);
	p.setPen(Qt::NoPen);

	const float radius = std::min(bh / 2.0f, fs * 0.9f);

	if (c.shadow && c.shadow_amount > 0) {
		// La sombra se dibuja aparte, se desenfoca y después se pega desplazada.
		QImage sh(img.size(), QImage::Format_ARGB32_Premultiplied);
		sh.fill(Qt::transparent);
		QPainter sp(&sh);
		sp.setRenderHint(QPainter::Antialiasing, true);
		sp.setPen(Qt::NoPen);
		sp.setBrush(QColor(0, 0, 0, 255));
		sp.drawRoundedRect(QRectF(pad, pad, bw, bh), radius, radius);
		sp.end();

		if (c.shadow_blur > 0)
			blur_alpha(sh, std::max(1, c.shadow_blur / 3));

		p.setOpacity(std::clamp(c.shadow_amount, 0, 100) / 100.0);
		p.drawImage(QPointF(dx, dy), sh);
		p.setOpacity(1.0);
	}

	p.setBrush(to_qcolor(c.bg));
	p.drawRoundedRect(QRectF(pad, pad, bw, bh), radius, radius);

	p.setFont(font);
	p.setPen(to_qcolor(c.fg));
	p.drawText(QRectF(pad + padx, pad + pady, textw, texth), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
		   text);
	p.end();

	out = img.convertToFormat(QImage::Format_RGBA8888);
	pad_out = pad;
	margin_out = margin;
}

void ensure_bubble(NotifFilter *f, const std::shared_ptr<const Config> &c, uint32_t w, uint32_t h)
{
	if (f->bubble && f->bub_cfg == c && f->bub_cw == w && f->bub_ch == h)
		return;

	QImage img;
	int pad = 0, margin = 0;
	render_bubble_image(*c, w, h, img, pad, margin);

	// Si la notificacion esta visible y cambia el tamaño, se anima de un tamaño al otro.
	const bool was_shown = f->bubble && f->bub_drawn && f->bub_cw == w && f->bub_ch == h &&
			       (f->state == State::In || f->state == State::Hold);
	const int prev_w = f->bub_w, prev_h = f->bub_h;

	if (f->old_tex) {
		gs_texture_destroy(f->old_tex);
		f->old_tex = nullptr;
	}
	if (f->bubble) {
		// Con la notificacion visible, la burbuja anterior se guarda para el fundido.
		if (was_shown && f->fade_effect)
			f->old_tex = f->bubble;
		else
			gs_texture_destroy(f->bubble);
		f->bubble = nullptr;
	}

	const uint8_t *data = img.constBits();
	f->bubble = gs_texture_create(uint32_t(img.width()), uint32_t(img.height()), GS_RGBA, 1, &data, 0);
	f->bub_w = img.width();
	f->bub_h = img.height();
	f->bub_pad = pad;
	f->bub_margin = margin;
	f->bub_cfg = c;
	f->bub_cw = w;
	f->bub_ch = h;

	if (was_shown) {
		f->old_w = prev_w;
		f->old_h = prev_h;
		f->morph = 0.0f;
	} else {
		f->morph = 1.0f;
	}
	f->bub_drawn = false;
}

// ---------------------------------------------------------------------------
// Desenfoque en dos pases
// ---------------------------------------------------------------------------
void draw_blurred(NotifFilter *f, uint32_t w, uint32_t h, float radius, int scale)
{
	// El desenfoque se hace sobre una imagen más chica (mucho más liviano para la placa de video)
	// y al final se estira al tamaño completo. Con desenfoques grandes no se nota la diferencia.
	const uint32_t sw = std::max<uint32_t>(1, w / uint32_t(scale));
	const uint32_t sh = std::max<uint32_t>(1, h / uint32_t(scale));
	const float small_radius = std::max(1.0f, radius / float(scale));

	gs_eparam_t *p_image = gs_effect_get_param_by_name(f->effect, "image");
	gs_eparam_t *p_dir = gs_effect_get_param_by_name(f->effect, "dir");
	gs_eparam_t *p_rad = gs_effect_get_param_by_name(f->effect, "radius");

	if (!obs_source_process_filter_begin(f->source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	vec4 clear;
	vec4_zero(&clear);

	// Pasada 1: horizontal. Imagen completa de la fuente hacia la textura chica A.
	vec2 d;
	vec2_set(&d, 1.0f / float(w), 0.0f);
	gs_effect_set_vec2(p_dir, &d);
	gs_effect_set_float(p_rad, radius);

	gs_texrender_reset(f->rt);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin(f->rt, sw, sh)) {
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, float(sw), 0.0f, float(sh), -100.0f, 100.0f);
		obs_source_process_filter_end(f->source, f->effect, sw, sh);
		gs_texrender_end(f->rt);
	}
	gs_blend_state_pop();

	gs_texture_t *tex_a = gs_texrender_get_texture(f->rt);
	if (!tex_a)
		return;

	// Pasada 2: vertical. De la textura A a la textura B (las dos chicas).
	vec2_set(&d, 0.0f, 1.0f / float(sh));
	gs_texrender_reset(f->rt2);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin(f->rt2, sw, sh)) {
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, float(sw), 0.0f, float(sh), -100.0f, 100.0f);
		gs_effect_set_vec2(p_dir, &d);
		gs_effect_set_float(p_rad, small_radius);
		gs_effect_set_texture_srgb(p_image, tex_a);
		while (gs_effect_loop(f->effect, "Draw"))
			gs_draw_sprite(tex_a, 0, sw, sh);
		gs_texrender_end(f->rt2);
	}
	gs_blend_state_pop();

	gs_texture_t *tex_b = gs_texrender_get_texture(f->rt2);
	if (!tex_b)
		return;

	// Pasada 3: la textura B se estira al tamaño completo de la fuente.
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *def_image = gs_effect_get_param_by_name(def, "image");
	gs_effect_set_texture_srgb(def_image, tex_b);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex_b, 0, w, h);
}

void draw_bubble(NotifFilter *f, const std::shared_ptr<const Config> &c, uint32_t w, uint32_t h, float e)
{
	ensure_bubble(f, c, w, h);
	if (!f->bubble)
		return;

	const float rest_x = float(f->bub_margin - f->bub_pad);
	const float rest_y = float(f->bub_margin - f->bub_pad);
	float x = rest_x, y = rest_y;

	// Tamaño mostrado: pasa del anterior al nuevo con la misma curva suave de la entrada.
	const float m = f->morph;
	const float em = m * m * (3.0f - 2.0f * m);
	const float cur_w = float(f->old_w) + float(f->bub_w - f->old_w) * em;
	const float cur_h = float(f->old_h) + float(f->bub_h - f->old_h) * em;
	const bool morphing = f->morph < 1.0f;
	const float draw_w = morphing ? cur_w : float(f->bub_w);
	const float draw_h = morphing ? cur_h : float(f->bub_h);

	if (c->direction == 1) {
		const float start = -draw_w;
		x = start + (rest_x - start) * e;
	} else if (c->direction == 2) {
		const float start = float(w);
		x = start + (rest_x - start) * e;
	} else {
		const float start = -draw_h;
		y = start + (rest_y - start) * e;
	}

	const uint32_t sw = uint32_t(std::max(1.0f, draw_w));
	const uint32_t sh = uint32_t(std::max(1.0f, draw_h));

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);

	if (morphing && f->old_tex && f->fade_effect) {
		// Fundido: primero la burbuja vieja completa y encima la nueva, cada vez mas visible.
		gs_eparam_t *p_img = gs_effect_get_param_by_name(f->fade_effect, "image");
		gs_eparam_t *p_alpha = gs_effect_get_param_by_name(f->fade_effect, "alpha");

		gs_effect_set_texture_srgb(p_img, f->old_tex);
		gs_effect_set_float(p_alpha, 1.0f);
		while (gs_effect_loop(f->fade_effect, "Draw"))
			gs_draw_sprite(f->old_tex, 0, sw, sh);

		gs_effect_set_texture_srgb(p_img, f->bubble);
		gs_effect_set_float(p_alpha, em);
		while (gs_effect_loop(f->fade_effect, "Draw"))
			gs_draw_sprite(f->bubble, 0, sw, sh);
	} else {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
		gs_effect_set_texture_srgb(img, f->bubble);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(f->bubble, 0, sw, sh);
	}

	gs_matrix_pop();
	gs_blend_state_pop();
	f->bub_drawn = true;

	// Terminado el fundido, la burbuja vieja ya no hace falta.
	if (!morphing && f->old_tex) {
		gs_texture_destroy(f->old_tex);
		f->old_tex = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Callbacks del filtro
// ---------------------------------------------------------------------------
const char *nf_name(void *)
{
	return "Notificación emergente";
}

void proc_show(void *data, calldata_t *)
{
	static_cast<NotifFilter *>(data)->want_show = true;
}

void proc_hide(void *data, calldata_t *)
{
	static_cast<NotifFilter *>(data)->want_hide = true;
}

void proc_toggle(void *data, calldata_t *)
{
	static_cast<NotifFilter *>(data)->want_toggle = true;
}

void proc_is_visible(void *data, calldata_t *cd)
{
	calldata_set_bool(cd, "visible", static_cast<NotifFilter *>(data)->visible.load());
}

obs_properties_t *nf_properties(void *)
{
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_text(p, "info",
				"Los ajustes son generales (iguales para todas las fuentes). Se cambian desde el dock \"Notificación emergente\".",
				OBS_TEXT_INFO);
	return p;
}

void *nf_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new NotifFilter();
	f->source = source;

	char *path = obs_module_file("effects/blur.effect");
	obs_enter_graphics();
	if (path)
		f->effect = gs_effect_create_from_file(path, nullptr);
	f->rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->rt2 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	char *fade_err = nullptr;
	f->fade_effect = gs_effect_create(kFadeEffect, "notif-fade.effect", &fade_err);
	if (!f->fade_effect)
		blog(LOG_WARNING, "[notif-popup] no se pudo crear el efecto de fundido: %s", fade_err ? fade_err : "");
	bfree(fade_err);
	obs_leave_graphics();
	if (!f->effect)
		blog(LOG_WARNING, "[notif-popup] no se pudo cargar effects/blur.effect");
	bfree(path);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void show()", proc_show, f);
	proc_handler_add(ph, "void hide()", proc_hide, f);
	proc_handler_add(ph, "void toggle()", proc_toggle, f);
	proc_handler_add(ph, "void is_visible(out bool visible)", proc_is_visible, f);

	(void)settings;
	return f;
}

void nf_destroy(void *data)
{
	auto *f = static_cast<NotifFilter *>(data);
	obs_enter_graphics();
	if (f->effect)
		gs_effect_destroy(f->effect);
	if (f->rt)
		gs_texrender_destroy(f->rt);
	if (f->rt2)
		gs_texrender_destroy(f->rt2);
	if (f->bubble)
		gs_texture_destroy(f->bubble);
	if (f->old_tex)
		gs_texture_destroy(f->old_tex);
	if (f->fade_effect)
		gs_effect_destroy(f->fade_effect);
	obs_leave_graphics();
	delete f;
}

void nf_tick(void *data, float dt)
{
	auto *f = static_cast<NotifFilter *>(data);
	auto c = get_cfg(f);

	if (f->want_show.exchange(false)) {
		if (f->state == State::Idle || f->state == State::Out) {
			f->state = State::In;
			f->hold_t = 0.0f;
		}
	}
	if (f->want_hide.exchange(false)) {
		if (f->state == State::In || f->state == State::Hold)
			f->state = State::Out;
	}
	if (f->want_toggle.exchange(false)) {
		if (f->state == State::In || f->state == State::Hold) {
			f->state = State::Out;
		} else {
			f->state = State::In;
			f->hold_t = 0.0f;
		}
	}

	// La transicion de tamaño dura lo mismo que la entrada.
	if (f->morph < 1.0f)
		// Armar una burbuja grande tarda y deja un cuadro largo; se limita dt para que esa pausa
		// no se coma el inicio de la transicion (por eso al crecer parecia un corte).
		f->morph = std::min(1.0f, f->morph + std::min(dt, 1.0f / 30.0f) / c->anim_in_sec);
	if (f->state == State::Idle) {
		f->bub_drawn = false;
		f->morph = 1.0f;
	}

	switch (f->state) {
	case State::In:
		f->prog += dt / c->anim_in_sec;
		if (f->prog >= 1.0f) {
			f->prog = 1.0f;
			f->state = State::Hold;
			f->hold_t = 0.0f;
		}
		break;
	case State::Hold:
		if (c->mode == 0 && c->hold_sec > 0.0f) {
			f->hold_t += dt;
			if (f->hold_t >= c->hold_sec)
				f->state = State::Out;
		}
		break;
	case State::Out:
		f->prog -= dt / c->anim_out_sec;
		if (f->prog <= 0.0f) {
			f->prog = 0.0f;
			f->state = State::Idle;
		}
		break;
	case State::Idle:
		break;
	}

	f->visible = (f->state == State::In || f->state == State::Hold);
}

void nf_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<NotifFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->source);

	if (!target || f->state == State::Idle) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h || !f->effect || !f->rt || !f->rt2) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	auto c = get_cfg(f);

	// Misma curva para entrar y para salir: arranca y frena suave, y la salida es el espejo de la entrada.
	const float e = f->prog * f->prog * (3.0f - 2.0f * f->prog);
	const float radius = c->blur_max * e * (float(h) / 1080.0f);

	// Cuanto más grande el desenfoque final, más se achica la imagen que se desenfoca.
	const float max_radius = c->blur_max * (float(h) / 1080.0f);
	const int scale = max_radius > 24.0f ? 4 : (max_radius > 8.0f ? 2 : 1);

	const bool prev = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	if (radius > 0.5f)
		draw_blurred(f, w, h, radius, scale);
	else
		obs_source_skip_video_filter(f->source);

	draw_bubble(f, c, w, h, e);

	gs_enable_framebuffer_srgb(prev);
}

} // namespace

// ---------------------------------------------------------------------------
// Ajustes generales: son los mismos para todas las fuentes y se guardan en un archivo propio
// ---------------------------------------------------------------------------
namespace {

std::mutex g_cfg_mtx;
obs_data_t *g_cfg = nullptr;
std::atomic<uint64_t> g_cfg_version{1};
bool g_cfg_fresh = true;

void set_config_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "text", "Escribe aquí tu frase");
	obs_data_set_default_int(s, "direction", 0);
	obs_data_set_default_double(s, "anim_in_sec", 1.0);
obs_data_set_default_double(s, "anim_out_sec", 1.0);
	obs_data_set_default_double(s, "hold_sec", 5.0);
	obs_data_set_default_int(s, "mode", 0);
	obs_data_set_default_double(s, "blur_max", 30.0);
	obs_data_set_default_string(s, "font_family", "Arial");
	obs_data_set_default_string(s, "font_file", "");
	obs_data_set_default_int(s, "font_size", 42);
	obs_data_set_default_bool(s, "bold", true);
	obs_data_set_default_int(s, "bg_color", 0xFFFF4010);
	obs_data_set_default_int(s, "text_color", 0xFFFFFFFF);
	obs_data_set_default_bool(s, "shadow", false);
	obs_data_set_default_int(s, "shadow_angle", 90);
	obs_data_set_default_int(s, "shadow_len", 16);
	obs_data_set_default_int(s, "shadow_blur", 24);
	obs_data_set_default_int(s, "shadow_amount", 50);
	obs_data_set_default_int(s, "phrase_sel", 1);
}

// Guarda el archivo de ajustes. Hay que llamarla con g_cfg_mtx tomado.
void save_config_locked()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("config.json");
	if (path) {
		obs_data_save_json_safe(g_cfg, path, "tmp", "bak");
		bfree(path);
	}
}

} // namespace

void notif_config_load()
{
	std::lock_guard<std::mutex> lock(g_cfg_mtx);
	if (g_cfg)
		return;

	char *path = obs_module_config_path("config.json");
	obs_data_t *data = path ? obs_data_create_from_json_file(path) : nullptr;
	bfree(path);

	g_cfg_fresh = (data == nullptr);
	g_cfg = data ? data : obs_data_create();
	set_config_defaults(g_cfg);

	// Si hay una fuente de letra agregada a mano, se vuelve a registrar al iniciar.
	register_font_file(obs_data_get_string(g_cfg, "font_file"));
}

uint64_t notif_config_version()
{
	return g_cfg_version.load();
}

void notif_config_read(const std::function<void(obs_data_t *)> &fn)
{
	std::lock_guard<std::mutex> lock(g_cfg_mtx);
	if (g_cfg)
		fn(g_cfg);
}

void notif_config_update(obs_data_t *changes)
{
	{
		std::lock_guard<std::mutex> lock(g_cfg_mtx);
		if (!g_cfg)
			return;
		obs_data_apply(g_cfg, changes);
		g_cfg_fresh = false;
		save_config_locked();
	}
	g_cfg_version++;
}

bool notif_config_is_fresh()
{
	std::lock_guard<std::mutex> lock(g_cfg_mtx);
	return g_cfg_fresh;
}

// Copia los ajustes que ya tenía guardados un filtro (versiones anteriores) a la configuración general.
void notif_config_import_from(obs_data_t *src)
{
	if (!src)
		return;

	{
		std::lock_guard<std::mutex> lock(g_cfg_mtx);
		if (!g_cfg)
			return;

		static const char *const int_keys[] = {"direction",    "font_size",   "bg_color",
						       "text_color",   "shadow_angle", "shadow_len",
						       "shadow_blur",  "shadow_amount", "mode",
						       "phrase_sel"};
		static const char *const double_keys[] = {"anim_in_sec", "anim_out_sec", "hold_sec", "blur_max"};
		static const char *const bool_keys[] = {"bold", "shadow"};
		static const char *const string_keys[] = {"text", "font_family", "font_file"};

		for (const char *k : int_keys)
			if (obs_data_has_user_value(src, k))
				obs_data_set_int(g_cfg, k, obs_data_get_int(src, k));
		for (const char *k : double_keys)
			if (obs_data_has_user_value(src, k))
				obs_data_set_double(g_cfg, k, obs_data_get_double(src, k));
		for (const char *k : bool_keys)
			if (obs_data_has_user_value(src, k))
				obs_data_set_bool(g_cfg, k, obs_data_get_bool(src, k));
		for (const char *k : string_keys)
			if (obs_data_has_user_value(src, k))
				obs_data_set_string(g_cfg, k, obs_data_get_string(src, k));
		for (int i = 1; i <= 9; i++) {
			char key[16];
			std::snprintf(key, sizeof(key), "phrase_%d", i);
			if (obs_data_has_user_value(src, key))
				obs_data_set_string(g_cfg, key, obs_data_get_string(src, key));
		}

		g_cfg_fresh = false;
		save_config_locked();
	}
	g_cfg_version++;
}

void register_notif_filter()
{
	obs_source_info info = {};
	info.id = NOTIF_FILTER_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = nf_name;
	info.create = nf_create;
	info.destroy = nf_destroy;
	info.get_properties = nf_properties;
	info.video_tick = nf_tick;
	info.video_render = nf_render;
	obs_register_source(&info);
}
