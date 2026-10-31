#pragma once

#include <obs-module.h>

#include <cstdint>
#include <functional>

#define NOTIF_FILTER_ID "notif_popup_filter"

// Registra el filtro de video "Notificación emergente".
void register_notif_filter();

// Ajustes generales: son los mismos para todas las fuentes y se guardan en un archivo propio del plugin.
void notif_config_load();
uint64_t notif_config_version();
void notif_config_read(const std::function<void(obs_data_t *)> &fn);
void notif_config_update(obs_data_t *changes);
bool notif_config_is_fresh();
void notif_config_import_from(obs_data_t *src);
