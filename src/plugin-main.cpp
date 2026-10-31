#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QMetaObject>
#include <QPointer>

#include "notif-filter.hpp"
#include "notif-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-notif-popup", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Notificación emergente estilo iPhone con desenfoque de fondo";
}

static QPointer<NotifDock> g_dock;
static obs_hotkey_id g_hotkey = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hotkey_bible = OBS_INVALID_HOTKEY_ID;

// Atajo "Acción de notificación": se ejecuta en otro hilo, así que se pasa al hilo de la interfaz.
static void on_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || g_dock.isNull())
		return;
	QMetaObject::invokeMethod(g_dock.data(), "toggleSelected", Qt::QueuedConnection);
}

// Atajo "Abrir Biblia" (F5 por defecto): abre la ventana de la Biblia.
static void on_hotkey_bible(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || g_dock.isNull())
		return;
	QMetaObject::invokeMethod(g_dock.data(), "openBible", Qt::QueuedConnection);
}

// Menú Herramientas > Abrir Biblia
static void on_tools_menu(void *)
{
	if (g_dock.isNull())
		return;
	QMetaObject::invokeMethod(g_dock.data(), "openBible", Qt::QueuedConnection);
}

// Guarda y carga las teclas elegidas junto con la colección de escenas.
static void save_hotkey(obs_data_t *save_data, bool saving, obs_hotkey_id id, const char *key)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return;

	if (saving) {
		obs_data_array_t *arr = obs_hotkey_save(id);
		obs_data_set_array(save_data, key, arr);
		obs_data_array_release(arr);
	} else {
		obs_data_array_t *arr = obs_data_get_array(save_data, key);
		if (arr) {
			obs_hotkey_load(id, arr);
			obs_data_array_release(arr);
		}
	}
}

static void on_save(obs_data_t *save_data, bool saving, void *)
{
	save_hotkey(save_data, saving, g_hotkey, "notif_popup_hotkey");
	save_hotkey(save_data, saving, g_hotkey_bible, "notif_popup_hotkey_bible");
}

bool obs_module_load(void)
{
	// Ajustes generales (iguales para todas las fuentes), guardados en un archivo propio.
	notif_config_load();

	register_notif_filter();

	g_hotkey = obs_hotkey_register_frontend("notif_popup.toggle", "Acción de notificación", on_hotkey, nullptr);

	// "Abrir Biblia": F5 por defecto; se puede cambiar en Ajustes > Atajos de teclado.
	g_hotkey_bible = obs_hotkey_register_frontend("notif_popup.open_bible", "Abrir Biblia", on_hotkey_bible, nullptr);
	obs_key_combination_t f5 = {0, OBS_KEY_F5};
	obs_hotkey_load_bindings(g_hotkey_bible, &f5, 1);
	obs_frontend_add_save_callback(on_save, nullptr);
	return true;
}

// El dock se crea acá, antes de que OBS restaure la posición de los paneles al iniciar,
// para que se acuerde de dónde estaba anclado.
extern "C" MODULE_EXPORT void obs_module_post_load(void)
{
	auto *dock = new NotifDock();
	g_dock = dock;
	obs_frontend_add_dock_by_id("notif_popup_dock", "Notificación emergente", dock);

	// Menú Herramientas > Abrir Biblia
	obs_frontend_add_tools_menu_item("Abrir Biblia", on_tools_menu, nullptr);
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(on_save, nullptr);
	if (g_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_hotkey);
	if (g_hotkey_bible != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_hotkey_bible);
}
