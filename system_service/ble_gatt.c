/* ble_gatt — BlueZ GATT server for a Nordic UART Service (NUS) over BLE.
 *
 * What this thread does, end-to-end:
 *   1. Connect to the system D-Bus.
 *   2. Discover the BT adapter (org.bluez.Adapter1, typically /org/bluez/hci0).
 *   3. Register an Agent1 with NoInputNoOutput capability -> Just-Works
 *      pairing (phone shows "Pair?" dialog, one tap, done, no PIN).
 *   4. Turn adapter on + pairable + discoverable (no timeout) and rename
 *      it to DEVICE_NAME so the phone shows "TAISHAN-PAI" in scan results.
 *   5. Export a GATT application tree on our bus connection:
 *        /com/taishan/ble                     ObjectManager
 *          /service0                          GattService1  (NUS, primary)
 *            /rx                              GattCharacteristic1 (write)
 *            /tx                              GattCharacteristic1 (notify)
 *      Call org.bluez.GattManager1.RegisterApplication — BlueZ walks the
 *      tree via GetManagedObjects and exposes our service to peers.
 *   6. Export an LEAdvertisement1 and call RegisterAdvertisement so the
 *      phone can find us in the first place.
 *
 * Message flow:
 *   phone --write--> RX.WriteValue()  -> we log + echo back via TX
 *   device --notify--> emit PropertiesChanged(Value) on TX  -> phone gets it
 *
 * Verbose by design: every D-Bus call, every incoming/outgoing byte, and
 * every BlueZ state transition is logged at stderr with an [ble] tag, so
 * you can tail the console on the debug UART and see exactly what the
 * stack is doing. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <pthread.h>

#include <gio/gio.h>

#define LOG_TAG "ble"
#include "log.h"

#include "ble_gatt.h"
#include "platform.h"
#include "protocol.h"
#include "wifi_switch.h"
#include "proto_sink.h"
#include "proto_dispatch.h"

/* =============================== Config =============================== */

#define DEVICE_NAME         SYSTEM_SERVICE_BLE_NAME

/* Nordic UART Service (de-facto standard for BLE "serial"). */
#define NUS_SERVICE_UUID    "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_CHAR_UUID    "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  /* phone -> device */
#define NUS_TX_CHAR_UUID    "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  /* device -> phone */

#define APP_ROOT_PATH       "/com/taishan/ble"
#define SERVICE_PATH        APP_ROOT_PATH "/service0"
#define RX_CHAR_PATH        SERVICE_PATH "/rx"
#define TX_CHAR_PATH        SERVICE_PATH "/tx"
#define ADV_PATH            APP_ROOT_PATH "/adv0"
#define AGENT_PATH          APP_ROOT_PATH "/agent"

/* =============================== Logging =============================== */

/* LOGI/LOGE/LOGW/LOGD come from log.h; LOG_TAG is "ble" for this file. */

static void log_hex(const char *prefix, const uint8_t *data, size_t len)
{
	char line[3 * 16 + 1];
	size_t i, j, n;

	if (len == 0) {
		LOGI("%s(empty)", prefix);
		return;
	}
	for (i = 0; i < len; i += 16) {
		n = (len - i < 16) ? (len - i) : 16;
		for (j = 0; j < n; j++)
			snprintf(line + j * 3, sizeof(line) - j * 3, "%02x ", data[i + j]);
		line[n * 3] = '\0';
		LOGI("%s[%04zu] %s", prefix, i, line);
	}
}

static void log_ascii(const char *prefix, const uint8_t *data, size_t len)
{
	size_t i;
	int printable = 1;

	for (i = 0; i < len; i++) {
		unsigned c = data[i];
		if ((c < 0x20 || c > 0x7e) && c != '\r' && c != '\n' && c != '\t') {
			printable = 0;
			break;
		}
	}
	if (printable && len > 0) {
		gchar *s = g_strndup((const gchar *)data, len);
		LOGI("%s\"%s\"", prefix, s);
		g_free(s);
	}
}

/* =============================== State =============================== */

typedef struct {
	const char *uuid;
	const char *const *flags;   /* NULL-terminated string list */
	int is_tx;                  /* 1 => notify direction, 0 => write direction */
} char_cfg_t;

static const char *rx_flags[] = { "write", "write-without-response", NULL };
static const char *tx_flags[] = { "notify", NULL };

static char_cfg_t g_rx_cfg = { NUS_RX_CHAR_UUID, rx_flags, 0 };
static char_cfg_t g_tx_cfg = { NUS_TX_CHAR_UUID, tx_flags, 1 };

static GDBusConnection *s_conn = NULL;
static GMainLoop       *s_loop = NULL;
static char            *s_adapter_path = NULL;

static GByteArray *s_rx_value = NULL;
static GByteArray *s_tx_value = NULL;
static gboolean    s_tx_notifying = FALSE;

static GDBusNodeInfo *s_agent_node   = NULL;
static GDBusNodeInfo *s_service_node = NULL;
static GDBusNodeInfo *s_char_node    = NULL;
static GDBusNodeInfo *s_adv_node     = NULL;
static GDBusNodeInfo *s_om_node      = NULL;

/* =========================== Introspection XML =========================== */

static const char agent_xml[] =
"<node>"
 "<interface name='org.bluez.Agent1'>"
  "<method name='Release'/>"
  "<method name='RequestPinCode'>"
   "<arg type='o' name='device' direction='in'/>"
   "<arg type='s' name='pincode' direction='out'/>"
  "</method>"
  "<method name='DisplayPinCode'>"
   "<arg type='o' name='device' direction='in'/>"
   "<arg type='s' name='pincode' direction='in'/>"
  "</method>"
  "<method name='RequestPasskey'>"
   "<arg type='o' name='device' direction='in'/>"
   "<arg type='u' name='passkey' direction='out'/>"
  "</method>"
  "<method name='DisplayPasskey'>"
   "<arg type='o' name='device' direction='in'/>"
   "<arg type='u' name='passkey' direction='in'/>"
   "<arg type='q' name='entered' direction='in'/>"
  "</method>"
  "<method name='RequestConfirmation'>"
   "<arg type='o' name='device' direction='in'/>"
   "<arg type='u' name='passkey' direction='in'/>"
  "</method>"
  "<method name='RequestAuthorization'>"
   "<arg type='o' name='device' direction='in'/>"
  "</method>"
  "<method name='AuthorizeService'>"
   "<arg type='o' name='device' direction='in'/>"
   "<arg type='s' name='uuid' direction='in'/>"
  "</method>"
  "<method name='Cancel'/>"
 "</interface>"
"</node>";

static const char service_xml[] =
"<node>"
 "<interface name='org.bluez.GattService1'>"
  "<property name='UUID' type='s' access='read'/>"
  "<property name='Primary' type='b' access='read'/>"
 "</interface>"
"</node>";

static const char char_xml[] =
"<node>"
 "<interface name='org.bluez.GattCharacteristic1'>"
  "<method name='ReadValue'>"
   "<arg type='a{sv}' name='options' direction='in'/>"
   "<arg type='ay' name='value' direction='out'/>"
  "</method>"
  "<method name='WriteValue'>"
   "<arg type='ay' name='value' direction='in'/>"
   "<arg type='a{sv}' name='options' direction='in'/>"
  "</method>"
  "<method name='StartNotify'/>"
  "<method name='StopNotify'/>"
  "<property name='UUID' type='s' access='read'/>"
  "<property name='Service' type='o' access='read'/>"
  "<property name='Value' type='ay' access='read'/>"
  "<property name='Flags' type='as' access='read'/>"
  "<property name='Notifying' type='b' access='read'/>"
 "</interface>"
"</node>";

static const char adv_xml[] =
"<node>"
 "<interface name='org.bluez.LEAdvertisement1'>"
  "<method name='Release'/>"
  "<property name='Type' type='s' access='read'/>"
  "<property name='ServiceUUIDs' type='as' access='read'/>"
  "<property name='LocalName' type='s' access='read'/>"
 "</interface>"
"</node>";

static const char om_xml[] =
"<node>"
 "<interface name='org.freedesktop.DBus.ObjectManager'>"
  "<method name='GetManagedObjects'>"
   "<arg type='a{oa{sa{sv}}}' name='objects' direction='out'/>"
  "</method>"
  "<signal name='InterfacesAdded'>"
   "<arg type='o' name='object'/>"
   "<arg type='a{sa{sv}}' name='interfaces'/>"
  "</signal>"
  "<signal name='InterfacesRemoved'>"
   "<arg type='o' name='object'/>"
   "<arg type='as' name='interfaces'/>"
  "</signal>"
 "</interface>"
"</node>";

/* =========================== Agent (Just-Works) =========================== */

static void agent_method(GDBusConnection *conn, const gchar *sender,
                         const gchar *object_path, const gchar *iface,
                         const gchar *method, GVariant *params,
                         GDBusMethodInvocation *inv, gpointer user_data)
{
	(void)conn; (void)object_path; (void)iface; (void)user_data;

	gchar *param_str = g_variant_print(params, TRUE);
	LOGI("Agent::%s  from=%s  params=%s", method, sender, param_str);
	g_free(param_str);

	/* NoInputNoOutput IO capability. BlueZ auto-picks Just-Works when
	 * both sides are NoInputNoOutput, so these confirmation callbacks
	 * all just return success without prompting a human. */
	if (g_strcmp0(method, "Release") == 0 ||
	    g_strcmp0(method, "Cancel") == 0 ||
	    g_strcmp0(method, "DisplayPinCode") == 0 ||
	    g_strcmp0(method, "DisplayPasskey") == 0 ||
	    g_strcmp0(method, "RequestConfirmation") == 0 ||
	    g_strcmp0(method, "RequestAuthorization") == 0 ||
	    g_strcmp0(method, "AuthorizeService") == 0) {
		LOGI("  -> auto-approve (Just-Works)");
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (g_strcmp0(method, "RequestPinCode") == 0) {
		/* Should not be reached with NoInputNoOutput, but be safe. */
		LOGI("  -> returning PIN 0000 (shouldn't happen w/ NoInputNoOutput)");
		g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "0000"));
	} else if (g_strcmp0(method, "RequestPasskey") == 0) {
		LOGI("  -> returning passkey 0");
		g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0u));
	} else {
		LOGE("  unknown agent method '%s'", method);
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
		    G_DBUS_ERROR_UNKNOWN_METHOD, "unknown agent method");
	}
}

static const GDBusInterfaceVTable agent_vtable = {
	.method_call  = agent_method,
	.get_property = NULL,
	.set_property = NULL,
};

/* =========================== GattService1 =========================== */

static GVariant *service_get(GDBusConnection *conn, const gchar *sender,
                             const gchar *path, const gchar *iface,
                             const gchar *prop, GError **error, gpointer ud)
{
	(void)conn; (void)sender; (void)path; (void)iface; (void)error; (void)ud;

	if (g_strcmp0(prop, "UUID") == 0)
		return g_variant_new_string(NUS_SERVICE_UUID);
	if (g_strcmp0(prop, "Primary") == 0)
		return g_variant_new_boolean(TRUE);
	return NULL;
}

static const GDBusInterfaceVTable service_vtable = {
	.method_call  = NULL,
	.get_property = service_get,
	.set_property = NULL,
};

/* =========================== GattCharacteristic1 =========================== */

static GVariant *char_value_variant(const char_cfg_t *cfg)
{
	GByteArray *val = cfg->is_tx ? s_tx_value : s_rx_value;
	return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
	                                 val ? val->data : NULL,
	                                 val ? val->len  : 0,
	                                 1);
}

static GVariant *char_flags_variant(const char_cfg_t *cfg)
{
	GVariantBuilder b;
	const char *const *f;

	g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
	for (f = cfg->flags; *f; f++)
		g_variant_builder_add(&b, "s", *f);
	return g_variant_builder_end(&b);
}

/* =========================== Protocol sink ===========================
 *
 * proto_dispatch.c does all the actual decoding, command handling, and
 * response generation now. We just provide a sink that wraps
 * ble_gatt_send so REQs received on the RX char get their RESPs (and
 * any async PUSHes from worker threads) emitted as TX notifications.
 *
 * Singleton — there is only one BLE TX channel, no per-connection
 * state to track, retain/release are no-ops. */
static void ble_sink_send(proto_sink_t *self, const uint8_t *data, size_t len)
{
	(void)self;
	ble_gatt_send(data, len);
}
static void ble_sink_noop(proto_sink_t *self) { (void)self; }
static proto_sink_t s_ble_sink = {
	.send    = ble_sink_send,
	.retain  = ble_sink_noop,
	.release = ble_sink_noop,
};

/* (frame send / dispatch / wifi connect / wifi scan handlers all moved
 * to proto_dispatch.c) */




static void char_method(GDBusConnection *conn, const gchar *sender,
                        const gchar *object_path, const gchar *iface,
                        const gchar *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer user_data)
{
	char_cfg_t *cfg = user_data;
	const char *tag = cfg->is_tx ? "TX" : "RX";

	(void)conn; (void)iface;

	LOGI("%s::%s  path=%s  sender=%s", tag, method, object_path, sender);

	if (g_strcmp0(method, "ReadValue") == 0) {
		GByteArray *val = cfg->is_tx ? s_tx_value : s_rx_value;
		LOGI("  ReadValue -> %u bytes", val ? val->len : 0);
		g_dbus_method_invocation_return_value(inv,
		    g_variant_new("(@ay)", char_value_variant(cfg)));
	} else if (g_strcmp0(method, "WriteValue") == 0) {
		GVariant *ay = NULL, *opts = NULL;
		gsize n = 0;
		const guint8 *data;

		g_variant_get(params, "(@ay@a{sv})", &ay, &opts);
		data = g_variant_get_fixed_array(ay, &n, sizeof(guint8));

		LOGI("  WriteValue: %zu bytes received", n);
		log_hex("    hex ", data, n);
		log_ascii("    txt ", data, n);

		if (!cfg->is_tx) {
			if (s_rx_value)
				g_byte_array_free(s_rx_value, TRUE);
			s_rx_value = g_byte_array_new();
			g_byte_array_append(s_rx_value, data, n);

			/* Hand the bytes off to the transport-agnostic
			 * dispatcher with our BLE sink. RESPs / async
			 * PUSHes will come back via ble_gatt_send. */
			proto_dispatch_buffer(data, n, &s_ble_sink);
		}

		g_variant_unref(ay);
		g_variant_unref(opts);
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (g_strcmp0(method, "StartNotify") == 0) {
		if (cfg->is_tx) {
			s_tx_notifying = TRUE;
			LOGI("  TX notifications: ENABLED (phone subscribed)");
		} else {
			LOGI("  StartNotify on RX ignored");
		}
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (g_strcmp0(method, "StopNotify") == 0) {
		if (cfg->is_tx) {
			s_tx_notifying = FALSE;
			LOGI("  TX notifications: disabled");
		}
		g_dbus_method_invocation_return_value(inv, NULL);
	} else {
		LOGE("  unknown characteristic method '%s'", method);
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
		    G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
	}
}

static GVariant *char_get(GDBusConnection *conn, const gchar *sender,
                          const gchar *path, const gchar *iface,
                          const gchar *prop, GError **error, gpointer ud)
{
	char_cfg_t *cfg = ud;

	(void)conn; (void)sender; (void)path; (void)iface; (void)error;

	if (g_strcmp0(prop, "UUID") == 0)
		return g_variant_new_string(cfg->uuid);
	if (g_strcmp0(prop, "Service") == 0)
		return g_variant_new_object_path(SERVICE_PATH);
	if (g_strcmp0(prop, "Value") == 0)
		return char_value_variant(cfg);
	if (g_strcmp0(prop, "Flags") == 0)
		return char_flags_variant(cfg);
	if (g_strcmp0(prop, "Notifying") == 0)
		return g_variant_new_boolean(cfg->is_tx ? s_tx_notifying : FALSE);
	return NULL;
}

static const GDBusInterfaceVTable char_vtable = {
	.method_call  = char_method,
	.get_property = char_get,
	.set_property = NULL,
};

/* =========================== LEAdvertisement1 =========================== */

static void adv_method(GDBusConnection *conn, const gchar *sender,
                       const gchar *path, const gchar *iface,
                       const gchar *method, GVariant *params,
                       GDBusMethodInvocation *inv, gpointer ud)
{
	(void)conn; (void)path; (void)iface; (void)params; (void)ud;
	LOGI("Advertisement::%s  from=%s", method, sender);
	g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *adv_get(GDBusConnection *conn, const gchar *sender,
                         const gchar *path, const gchar *iface,
                         const gchar *prop, GError **error, gpointer ud)
{
	(void)conn; (void)sender; (void)path; (void)iface; (void)error; (void)ud;

	if (g_strcmp0(prop, "Type") == 0)
		return g_variant_new_string("peripheral");
	if (g_strcmp0(prop, "LocalName") == 0)
		return g_variant_new_string(DEVICE_NAME);
	if (g_strcmp0(prop, "ServiceUUIDs") == 0) {
		GVariantBuilder b;
		g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
		g_variant_builder_add(&b, "s", NUS_SERVICE_UUID);
		return g_variant_builder_end(&b);
	}
	return NULL;
}

static const GDBusInterfaceVTable adv_vtable = {
	.method_call  = adv_method,
	.get_property = adv_get,
	.set_property = NULL,
};

/* =========================== ObjectManager =========================== */

/* Build the a{sv} property dict for our GattService1 object. */
static GVariant *props_service(void)
{
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "UUID",    g_variant_new_string(NUS_SERVICE_UUID));
	g_variant_builder_add(&b, "{sv}", "Primary", g_variant_new_boolean(TRUE));
	return g_variant_builder_end(&b);
}

/* Build the a{sv} property dict for one GattCharacteristic1 object. */
static GVariant *props_char(const char_cfg_t *cfg)
{
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "UUID",    g_variant_new_string(cfg->uuid));
	g_variant_builder_add(&b, "{sv}", "Service", g_variant_new_object_path(SERVICE_PATH));
	g_variant_builder_add(&b, "{sv}", "Flags",   char_flags_variant(cfg));
	g_variant_builder_add(&b, "{sv}", "Value",   char_value_variant(cfg));
	if (cfg->is_tx)
		g_variant_builder_add(&b, "{sv}", "Notifying",
		                      g_variant_new_boolean(s_tx_notifying));
	return g_variant_builder_end(&b);
}

/* Wrap {iface_name: props_dict} into an a{sa{sv}} with one entry. */
static GVariant *ifaces_one(const char *iface_name, GVariant *props)
{
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sa{sv}}"));
	g_variant_builder_add(&b, "{s@a{sv}}", iface_name, props);
	return g_variant_builder_end(&b);
}

static void om_method(GDBusConnection *conn, const gchar *sender,
                      const gchar *object_path, const gchar *iface,
                      const gchar *method, GVariant *params,
                      GDBusMethodInvocation *inv, gpointer ud)
{
	GVariantBuilder objs;

	(void)conn; (void)object_path; (void)iface; (void)params; (void)ud;

	if (g_strcmp0(method, "GetManagedObjects") != 0) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
		    G_DBUS_ERROR_UNKNOWN_METHOD, "unknown");
		return;
	}

	LOGI("ObjectManager::GetManagedObjects  from=%s  (BlueZ walking our GATT tree)", sender);

	g_variant_builder_init(&objs, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

	LOGI("  [1/3] %s  GattService1  uuid=%s  primary=true",
	     SERVICE_PATH, NUS_SERVICE_UUID);
	g_variant_builder_add(&objs, "{o@a{sa{sv}}}", SERVICE_PATH,
	                      ifaces_one("org.bluez.GattService1", props_service()));

	LOGI("  [2/3] %s  GattCharacteristic1  uuid=%s  flags=write,write-without-response",
	     RX_CHAR_PATH, NUS_RX_CHAR_UUID);
	g_variant_builder_add(&objs, "{o@a{sa{sv}}}", RX_CHAR_PATH,
	                      ifaces_one("org.bluez.GattCharacteristic1", props_char(&g_rx_cfg)));

	LOGI("  [3/3] %s  GattCharacteristic1  uuid=%s  flags=notify",
	     TX_CHAR_PATH, NUS_TX_CHAR_UUID);
	g_variant_builder_add(&objs, "{o@a{sa{sv}}}", TX_CHAR_PATH,
	                      ifaces_one("org.bluez.GattCharacteristic1", props_char(&g_tx_cfg)));

	g_dbus_method_invocation_return_value(inv,
	    g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objs)));
	LOGI("  -> replied 3 objects to BlueZ, RegisterApplication should now complete");
}

static const GDBusInterfaceVTable om_vtable = {
	.method_call  = om_method,
	.get_property = NULL,
	.set_property = NULL,
};

/* =========================== Public: ble_gatt_send =========================== */

void ble_gatt_send(const uint8_t *data, size_t len)
{
	GVariantBuilder changed, invalidated;
	GError *err = NULL;

	if (!s_conn) {
		LOGI("ble_gatt_send: dropped %zu bytes (D-Bus not ready)", len);
		return;
	}
	if (!s_tx_notifying) {
		LOGI("ble_gatt_send: dropped %zu bytes (no subscriber on TX)", len);
		return;
	}

	if (s_tx_value)
		g_byte_array_free(s_tx_value, TRUE);
	s_tx_value = g_byte_array_new();
	g_byte_array_append(s_tx_value, data, len);

	g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&changed, "{sv}", "Value",
	    g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, 1));

	g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));

	g_dbus_connection_emit_signal(s_conn, NULL, TX_CHAR_PATH,
	    "org.freedesktop.DBus.Properties", "PropertiesChanged",
	    g_variant_new("(sa{sv}as)", "org.bluez.GattCharacteristic1",
	                  &changed, &invalidated),
	    &err);

	if (err) {
		LOGE("emit PropertiesChanged: %s", err->message);
		g_error_free(err);
	} else {
		LOGI("TX notify: %zu bytes pushed to subscriber", len);
		log_hex("  hex ", data, len);
	}
}

/* =========================== Adapter helpers =========================== */

static char *find_adapter(void)
{
	GError *err = NULL;
	GVariant *res, *objs;
	GVariantIter iter;
	gchar *path = NULL;
	GVariant *ifaces = NULL;
	char *adapter = NULL;

	res = g_dbus_connection_call_sync(s_conn, "org.bluez", "/",
	    "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
	    NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
	    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!res) {
		LOGE("GetManagedObjects on /: %s", err ? err->message : "?");
		if (err) g_error_free(err);
		return NULL;
	}

	objs = g_variant_get_child_value(res, 0);
	g_variant_iter_init(&iter, objs);
	while (g_variant_iter_next(&iter, "{o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *adapter_if = g_variant_lookup_value(ifaces,
		                         "org.bluez.Adapter1", NULL);
		if (adapter_if) {
			adapter = g_strdup(path);
			g_variant_unref(adapter_if);
			g_free(path);
			g_variant_unref(ifaces);
			break;
		}
		g_free(path);
		g_variant_unref(ifaces);
	}
	g_variant_unref(objs);
	g_variant_unref(res);
	return adapter;
}

static void set_adapter_prop(const char *prop, GVariant *value)
{
	GError *err = NULL;
	GVariant *res;
	gchar *vs;

	vs = g_variant_print(value, TRUE);
	res = g_dbus_connection_call_sync(s_conn, "org.bluez", s_adapter_path,
	    "org.freedesktop.DBus.Properties", "Set",
	    g_variant_new("(ssv)", "org.bluez.Adapter1", prop, value),
	    NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!res) {
		LOGE("Adapter1.%s = %s  FAIL: %s", prop, vs,
		     err ? err->message : "?");
		if (err) g_error_free(err);
	} else {
		LOGI("Adapter1.%s = %s  OK", prop, vs);
		g_variant_unref(res);
	}
	g_free(vs);
}

/* =========================== Register with BlueZ =========================== */

static gboolean register_agent(void)
{
	GError *err = NULL;
	GVariant *res;
	gint64 t0;

	LOGI(">> AgentManager1.RegisterAgent(path=%s, capability=NoInputNoOutput)",
	     AGENT_PATH);
	t0 = g_get_monotonic_time();
	res = g_dbus_connection_call_sync(s_conn, "org.bluez", "/org/bluez",
	    "org.bluez.AgentManager1", "RegisterAgent",
	    g_variant_new("(os)", AGENT_PATH, "NoInputNoOutput"),
	    NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!res) {
		LOGE("<< RegisterAgent FAIL after %lld ms: %s",
		     (long long)((g_get_monotonic_time() - t0) / 1000),
		     err ? err->message : "?");
		if (err) g_error_free(err);
		return FALSE;
	}
	g_variant_unref(res);
	LOGI("<< RegisterAgent OK (%lld ms)",
	     (long long)((g_get_monotonic_time() - t0) / 1000));

	LOGI(">> AgentManager1.RequestDefaultAgent(%s)", AGENT_PATH);
	t0 = g_get_monotonic_time();
	res = g_dbus_connection_call_sync(s_conn, "org.bluez", "/org/bluez",
	    "org.bluez.AgentManager1", "RequestDefaultAgent",
	    g_variant_new("(o)", AGENT_PATH),
	    NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!res) {
		LOGE("<< RequestDefaultAgent FAIL after %lld ms: %s",
		     (long long)((g_get_monotonic_time() - t0) / 1000),
		     err ? err->message : "?");
		if (err) g_error_free(err);
		return FALSE;
	}
	g_variant_unref(res);
	LOGI("<< RequestDefaultAgent OK (%lld ms) — Just-Works pairing armed",
	     (long long)((g_get_monotonic_time() - t0) / 1000));
	return TRUE;
}

static gboolean register_gatt_app(void)
{
	GError *err = NULL;
	GVariantBuilder opts;
	GVariant *res;
	gint64 t0;

	LOGI(">> GattManager1.RegisterApplication  app=%s  adapter=%s",
	     APP_ROOT_PATH, s_adapter_path);
	LOGI("   (BlueZ will reverse-call GetManagedObjects on %s — main loop MUST be running)",
	     APP_ROOT_PATH);

	g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
	t0 = g_get_monotonic_time();
	res = g_dbus_connection_call_sync(s_conn, "org.bluez", s_adapter_path,
	    "org.bluez.GattManager1", "RegisterApplication",
	    g_variant_new("(oa{sv})", APP_ROOT_PATH, &opts),
	    NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!res) {
		gint64 ms = (g_get_monotonic_time() - t0) / 1000;
		LOGE("<< RegisterApplication FAIL after %lld ms: %s",
		     (long long)ms, err ? err->message : "?");
		if (err && err->message && strstr(err->message, "Timeout"))
			LOGE("   hint: timeout usually means BlueZ's GetManagedObjects "
			     "reverse-call could not be dispatched. Ensure the GMainLoop "
			     "is running on this thread BEFORE calling RegisterApplication, "
			     "and that om_method ran (look for 'ObjectManager::GetManagedObjects').");
		if (err) g_error_free(err);
		return FALSE;
	}
	g_variant_unref(res);
	LOGI("<< RegisterApplication OK (%lld ms) — GATT app is live on %s",
	     (long long)((g_get_monotonic_time() - t0) / 1000), s_adapter_path);
	return TRUE;
}

static gboolean register_advertisement(void)
{
	GError *err = NULL;
	GVariantBuilder opts;
	GVariant *res;
	gint64 t0;

	LOGI(">> LEAdvertisingManager1.RegisterAdvertisement  adv=%s  adapter=%s",
	     ADV_PATH, s_adapter_path);
	LOGI("   advertised: Type=peripheral  LocalName='%s'  ServiceUUIDs=[%s]",
	     DEVICE_NAME, NUS_SERVICE_UUID);

	g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
	t0 = g_get_monotonic_time();
	res = g_dbus_connection_call_sync(s_conn, "org.bluez", s_adapter_path,
	    "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement",
	    g_variant_new("(oa{sv})", ADV_PATH, &opts),
	    NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!res) {
		gint64 ms = (g_get_monotonic_time() - t0) / 1000;
		LOGE("<< RegisterAdvertisement FAIL after %lld ms: %s",
		     (long long)ms, err ? err->message : "?");
		LOGE("   hint: bluetoothd must be started with '-E' (experimental)"
		     " or LEAdvertisingManager1 is not exported.");
		LOGE("   hint: if err says 'Maximum advertisements reached', another "
		     "process already holds the ad slot — stop it first.");
		if (err) g_error_free(err);
		return FALSE;
	}
	g_variant_unref(res);
	LOGI("<< RegisterAdvertisement OK (%lld ms) — phone BLE scan should now see '%s'",
	     (long long)((g_get_monotonic_time() - t0) / 1000), DEVICE_NAME);
	return TRUE;
}

/* Register our D-Bus objects on our bus connection. BlueZ walks this
 * tree when we call RegisterApplication below. */
static gboolean register_objects(void)
{
	GError *err = NULL;

	s_agent_node   = g_dbus_node_info_new_for_xml(agent_xml, &err);
	if (!s_agent_node) goto xml_err;
	s_service_node = g_dbus_node_info_new_for_xml(service_xml, &err);
	if (!s_service_node) goto xml_err;
	s_char_node    = g_dbus_node_info_new_for_xml(char_xml, &err);
	if (!s_char_node) goto xml_err;
	s_adv_node     = g_dbus_node_info_new_for_xml(adv_xml, &err);
	if (!s_adv_node) goto xml_err;
	s_om_node      = g_dbus_node_info_new_for_xml(om_xml, &err);
	if (!s_om_node) goto xml_err;

#define REG(path, node, vtable, udata, tag) do { \
		guint id = g_dbus_connection_register_object(s_conn, \
		    (path), (node)->interfaces[0], (vtable), (udata), NULL, &err); \
		if (!id) { \
			LOGE("register %s at %s: %s", tag, path, \
			     err ? err->message : "?"); \
			if (err) { g_error_free(err); err = NULL; } \
			return FALSE; \
		} \
		LOGI("exported %s at %s  (reg_id=%u)", tag, path, id); \
	} while (0)

	REG(AGENT_PATH,   s_agent_node,   &agent_vtable,   NULL,      "Agent1");
	REG(SERVICE_PATH, s_service_node, &service_vtable, NULL,      "GattService1");
	REG(RX_CHAR_PATH, s_char_node,    &char_vtable,    &g_rx_cfg, "RX char");
	REG(TX_CHAR_PATH, s_char_node,    &char_vtable,    &g_tx_cfg, "TX char");
	REG(ADV_PATH,     s_adv_node,     &adv_vtable,     NULL,      "LEAdvertisement1");
	REG(APP_ROOT_PATH, s_om_node,     &om_vtable,      NULL,      "ObjectManager");
#undef REG

	return TRUE;

xml_err:
	LOGE("parse introspection xml: %s", err ? err->message : "?");
	if (err) g_error_free(err);
	return FALSE;
}

/* =========================== Thread entry =========================== */

/* Runs on a dedicated worker thread so the main thread can stay inside
 * g_main_loop_run() and pump incoming D-Bus calls.
 *
 * GattManager1.RegisterApplication is synchronous AND BlueZ reverse-calls
 * GetManagedObjects on our ObjectManager while that call is outstanding.
 * If we drove the sync call from the same thread that's pumping the main
 * loop (e.g. via g_idle_add), the call_sync would block the dispatcher
 * itself — BlueZ's GetManagedObjects would sit in s_conn's inbound queue
 * with nobody to deliver it, RegisterApplication would hit its 25 s timeout,
 * and only then (after the sync returned and the loop resumed iterating)
 * would GetManagedObjects finally fire — way too late.
 *
 * Splitting the responsibilities across two threads keeps the dispatcher
 * free: this worker does the sync calls, the main thread keeps iterating
 * the default GMainContext and answers BlueZ's reverse-calls in real time.
 * GDBusConnection is thread-safe so sharing s_conn across them is fine. */
static void *bluez_register_worker(void *ud)
{
	(void)ud;

	LOGI("-------- worker thread started; main loop pumping on main thread --------");

	LOGI("[step 1/3] register pairing agent");
	if (!register_agent())
		LOGE("  agent registration failed — pairing may still work, continuing");

	LOGI("[step 2/3] register GATT application (NUS service tree)");
	if (!register_gatt_app()) {
		LOGE("  GATT app registration failed — BLE NUS will not be reachable");
		LOGE("  keeping main loop alive so diagnostic logs stay available");
		return NULL;
	}

	LOGI("[step 3/3] register LE advertisement");
	if (!register_advertisement()) {
		LOGE("  LE advertising failed — phones will NOT see us in BLE scan");
		LOGE("  (classic BT inquiry may still find '%s' via Adapter1.Alias)",
		     DEVICE_NAME);
		return NULL;
	}

	LOGI("======== ble_gatt READY ========");
	LOGI("   adapter    : %s", s_adapter_path);
	LOGI("   advertising: LocalName='%s'  (peripheral, UUID=%s)",
	     DEVICE_NAME, NUS_SERVICE_UUID);
	LOGI("   RX char    : %s  (phone writes here)", NUS_RX_CHAR_UUID);
	LOGI("   TX char    : %s  (phone subscribes here)", NUS_TX_CHAR_UUID);
	LOGI("   waiting for phone connection...");
	return NULL;
}

static void platform_prepare_bluetooth(void)
{
#if SYSTEM_SERVICE_IS_A733
	/* Debian images commonly leave Bluetooth soft-blocked until userspace
	 * unblocks it. These commands are best-effort only; the real readiness
	 * check below is still BlueZ's org.bluez.Adapter1 discovery. */
	LOGI("[init] A733/Debian bluetooth prepare: rfkill unblock + hci up");
	system("rfkill unblock bluetooth >/dev/null 2>&1");
	system("hciconfig hci0 up >/dev/null 2>&1");
#endif
}

void *ble_gatt_thread(void *arg)
{
	GError *err = NULL;
	int retries;

	(void)arg;

	LOGI("======== ble_gatt_thread starting ========");
	LOGI("device name : %s", DEVICE_NAME);
	LOGI("service UUID: %s", NUS_SERVICE_UUID);
	LOGI("RX char UUID: %s  (phone writes here)", NUS_RX_CHAR_UUID);
	LOGI("TX char UUID: %s  (phone subscribes here)", NUS_TX_CHAR_UUID);

	platform_prepare_bluetooth();

	/* System bus: bluez runs there. Session bus wouldn't work. */
	LOGI("[init] connecting to system D-Bus...");
	s_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!s_conn) {
		LOGE("g_bus_get_sync(SYSTEM): %s",
		     err ? err->message : "?");
		if (err) g_error_free(err);
		return NULL;
	}
	LOGI("[init] connected to system D-Bus (unique name=%s)",
	     g_dbus_connection_get_unique_name(s_conn));

	/* BlueZ may take a few seconds to export the adapter after
	 * bluetoothd starts and hci0 comes up. Retry. */
	LOGI("[init] discovering org.bluez.Adapter1 ...");
	for (retries = 0; retries < 60; retries++) {
		s_adapter_path = find_adapter();
		if (s_adapter_path) break;
		LOGI("       waiting for adapter ... (%d/60)", retries + 1);
		sleep(2);
	}
	if (!s_adapter_path) {
		LOGE("no BlueZ adapter after 120s; is hci0 up? is bluetoothd running?");
		return NULL;
	}
	LOGI("[init] adapter found: %s", s_adapter_path);

	/* Adapter properties. Alias is what phones display; Pairable +
	 * Discoverable with zero timeout keeps us reachable indefinitely. */
	LOGI("[init] configuring Adapter1 properties (Alias/Powered/Pairable/Discoverable)");
	set_adapter_prop("Alias",               g_variant_new_string(DEVICE_NAME));
	set_adapter_prop("Powered",             g_variant_new_boolean(TRUE));
	set_adapter_prop("Pairable",            g_variant_new_boolean(TRUE));
	set_adapter_prop("PairableTimeout",     g_variant_new_uint32(0));
	set_adapter_prop("Discoverable",        g_variant_new_boolean(TRUE));
	set_adapter_prop("DiscoverableTimeout", g_variant_new_uint32(0));

	LOGI("[init] exporting our D-Bus object tree under %s", APP_ROOT_PATH);
	if (!register_objects())
		return NULL;

	/* IMPORTANT: drive RegisterApplication from a worker thread, NOT from
	 * the main loop thread (e.g. g_idle_add). BlueZ reverse-calls
	 * GetManagedObjects while RegisterApplication is in flight; if the
	 * sync call ran on the dispatcher thread it would block the loop and
	 * the reverse-call would time out. See bluez_register_worker(). */
	s_loop = g_main_loop_new(NULL, FALSE);

	{
		pthread_t reg_tid;
		LOGI("[init] spawning BlueZ registration worker thread");
		if (pthread_create(&reg_tid, NULL, bluez_register_worker, NULL) != 0) {
			LOGE("pthread_create(bluez_register_worker) failed; "
			     "falling back to in-loop registration (may deadlock)");
			bluez_register_worker(NULL);
		} else {
			pthread_detach(reg_tid);
		}
	}

	LOGI("[init] entering GMainLoop (pumping BlueZ reverse-calls on this thread)");
	g_main_loop_run(s_loop);

	/* Unreachable in normal operation; SIGTERM from start-stop-daemon
	 * kills the whole process and all threads together. */
	LOGI("main loop exited");
	return NULL;
}
