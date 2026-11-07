#include "arvfeaturetree.h"
#include <math.h>

enum {
	GROUP_COLUMN_NAME,
	GROUP_COLUMN_CATEGORY,
	N_GROUP_COLUMNS
};

struct _ArvViewerFeatureTree {
	GtkBox parent_instance;
	GtkWidget *container;
	GtkTreeStore *group_store;
	GtkWidget *group_tree;
	GtkWidget *group_label;
	GtkWidget *feature_list;
	GList *rows;
	ArvGc *genicam;
	GtkWidget *auto_refresh_button;
	guint auto_refresh_event;
	ArvViewerFeatureTreeChangeBeginFunc change_begin;
	ArvViewerFeatureTreeChangeEndFunc change_end;
	gpointer change_user_data;
	ArvViewerFeatureTreeRefreshFunc refresh_func;
	gpointer refresh_user_data;
};

G_DEFINE_TYPE (ArvViewerFeatureTree, arv_viewer_feature_tree, GTK_TYPE_BOX)

typedef struct {
	ArvGcFeatureNode *feature;
	ArvViewerFeatureTree *feature_tree;
	gboolean updating;
} FeatureControlData;

typedef struct {
	ArvGcFeatureNode *feature;
	GtkWidget *control;
	FeatureControlData *data;
} FeatureRow;

static void
feature_control_data_free (gpointer user_data, GClosure *closure)
{
	FeatureControlData *data = user_data;

	(void) closure;
	g_clear_object (&data->feature);
	g_free (data);
}

static FeatureControlData *
feature_control_data_new (ArvGcFeatureNode *feature, ArvViewerFeatureTree *feature_tree)
{
	FeatureControlData *data = g_new0 (FeatureControlData, 1);

	data->feature = g_object_ref (feature);
	data->feature_tree = feature_tree;
	return data;
}

static void
feature_row_free (FeatureRow *row)
{
	g_clear_object (&row->feature);
	g_free (row);
}

static gboolean
feature_is_editable (ArvGcFeatureNode *feature)
{
	GError *error = NULL;
	gboolean available;
	gboolean locked;
	ArvGcAccessMode access_mode;

	available = arv_gc_feature_node_is_implemented (feature, &error) &&
		arv_gc_feature_node_is_available (feature, &error);
	g_clear_error (&error);
	locked = arv_gc_feature_node_is_locked (feature, &error);
	g_clear_error (&error);
	access_mode = arv_gc_feature_node_get_actual_access_mode (feature);

	return available && !locked &&
		(access_mode == ARV_GC_ACCESS_MODE_WO || access_mode == ARV_GC_ACCESS_MODE_RW);
}

static gboolean
feature_get_numeric_range (ArvGcFeatureNode *feature,
			   double *minimum,
			   double *maximum,
			   double *increment)
{
	GError *error = NULL;

	if (ARV_IS_GC_INTEGER (feature)) {
		*minimum = arv_gc_integer_get_min (ARV_GC_INTEGER (feature), &error);
		if (error != NULL)
			goto fail;
		*maximum = arv_gc_integer_get_max (ARV_GC_INTEGER (feature), &error);
		if (error != NULL)
			goto fail;
		*increment = arv_gc_integer_get_inc (ARV_GC_INTEGER (feature), &error);
		if (error != NULL)
			goto fail;
	} else if (ARV_IS_GC_FLOAT (feature)) {
		*minimum = arv_gc_float_get_min (ARV_GC_FLOAT (feature), &error);
		if (error != NULL)
			goto fail;
		*maximum = arv_gc_float_get_max (ARV_GC_FLOAT (feature), &error);
		if (error != NULL)
			goto fail;
		*increment = arv_gc_float_get_inc (ARV_GC_FLOAT (feature), &error);
		if (error != NULL)
			goto fail;
	} else {
		return FALSE;
	}

	return isfinite (*minimum) && isfinite (*maximum) && *maximum > *minimum;

fail:
	g_clear_error (&error);
	return FALSE;
}

static char *
get_feature_value (ArvGcFeatureNode *feature)
{
	GError *error = NULL;
	const char *value;
	char *copy;

	if (arv_gc_feature_node_get_actual_access_mode (feature) == ARV_GC_ACCESS_MODE_WO)
		return g_strdup ("Write only");

	value = arv_gc_feature_node_get_value_as_string (feature, &error);
	copy = g_strdup (value != NULL ? value : "Unavailable");
	g_clear_error (&error);

	return copy;
}

static void
set_feature_value_from_control (ArvGcFeatureNode *feature, const char *value)
{
	GError *error = NULL;

	arv_gc_feature_node_set_value_from_string (feature, value, &error);
	if (error != NULL) {
		g_warning ("Unable to set GenICam feature '%s': %s",
			   arv_gc_feature_node_get_name (feature), error->message);
		g_clear_error (&error);
	}
}

/* Wrap a feature write so the camera can be stopped before and restored after,
 * as some features cannot be changed while acquisition is running. */
static gpointer
feature_change_begin (FeatureControlData *data)
{
	ArvViewerFeatureTree *feature_tree = data->feature_tree;

	if (feature_tree->change_begin != NULL)
		return feature_tree->change_begin (feature_tree->change_user_data);

	return NULL;
}

static void
feature_change_end (FeatureControlData *data, gpointer state)
{
	ArvViewerFeatureTree *feature_tree = data->feature_tree;

	if (feature_tree->change_end != NULL)
		feature_tree->change_end (feature_tree->change_user_data, state);
}

static void
numeric_value_changed_cb (GtkSpinButton *spin_button, FeatureControlData *data)
{
	GError *error = NULL;
	double value;
	gpointer state;

	if (data->updating)
		return;
	value = gtk_spin_button_get_value (spin_button);

	state = feature_change_begin (data);
	if (ARV_IS_GC_INTEGER (data->feature))
		arv_gc_integer_set_value (ARV_GC_INTEGER (data->feature), (gint64) llround (value), &error);
	else
		arv_gc_float_set_value (ARV_GC_FLOAT (data->feature), value, &error);
	feature_change_end (data, state);

	if (error != NULL) {
		g_warning ("Unable to set GenICam feature '%s': %s",
			   arv_gc_feature_node_get_name (data->feature), error->message);
		g_clear_error (&error);
	}
}

static void
boolean_toggled_cb (GtkToggleButton *button, FeatureControlData *data)
{
	GError *error = NULL;
	gpointer state;

	if (data->updating)
		return;
	state = feature_change_begin (data);
	arv_gc_boolean_set_value (ARV_GC_BOOLEAN (data->feature),
				  gtk_toggle_button_get_active (button), &error);
	feature_change_end (data, state);
	if (error != NULL) {
		g_warning ("Unable to set GenICam feature '%s': %s",
			   arv_gc_feature_node_get_name (data->feature), error->message);
		g_clear_error (&error);
	}
}

static void
enum_changed_cb (GtkComboBox *combo, FeatureControlData *data)
{
	const gchar *value;
	gpointer state;

	if (data->updating)
		return;
	value = gtk_combo_box_get_active_id (combo);
	if (value != NULL) {
		state = feature_change_begin (data);
		set_feature_value_from_control (data->feature, value);
		feature_change_end (data, state);
	}
}

static void
string_entry_activated_cb (GtkEntry *entry, FeatureControlData *data)
{
	gpointer state;

	state = feature_change_begin (data);
	set_feature_value_from_control (data->feature, gtk_entry_get_text (entry));
	feature_change_end (data, state);
}

static void
command_clicked_cb (GtkButton *button, FeatureControlData *data)
{
	GError *error = NULL;
	gpointer state;

	(void) button;
	state = feature_change_begin (data);
	arv_gc_command_execute (ARV_GC_COMMAND (data->feature), &error);
	feature_change_end (data, state);
	if (error != NULL) {
		g_warning ("Unable to execute GenICam command '%s': %s",
			   arv_gc_feature_node_get_name (data->feature), error->message);
		g_clear_error (&error);
	}
}

static GtkWidget *
create_feature_control (ArvViewerFeatureTree *feature_tree, ArvGcFeatureNode *feature,
			FeatureControlData **out_data)
{
	GtkWidget *control;
	FeatureControlData *data;
	GError *error = NULL;
	char *value;
	gboolean editable = feature_is_editable (feature);

	*out_data = NULL;

	if (ARV_IS_GC_COMMAND (feature)) {
		control = gtk_button_new_with_label ("Execute");
		data = feature_control_data_new (feature, feature_tree);
		g_signal_connect_data (control, "clicked", G_CALLBACK (command_clicked_cb), data,
				      feature_control_data_free, 0);
		gtk_widget_set_sensitive (control, editable);
		*out_data = data;
		return control;
	}

	if (ARV_IS_GC_INTEGER (feature) || ARV_IS_GC_FLOAT (feature)) {
		double minimum, maximum, increment, current_value;
		GtkAdjustment *adjustment;
		gint digits = 0;

		if (feature_get_numeric_range (feature, &minimum, &maximum, &increment)) {
			if (increment <= 0.0 || !isfinite (increment))
				increment = ARV_IS_GC_INTEGER (feature) ? 1.0 : (maximum - minimum) / 100.0;
			value = get_feature_value (feature);
			current_value = g_ascii_strtod (value, NULL);
			g_free (value);
			if (!isfinite (current_value))
				current_value = minimum;
			current_value = CLAMP (current_value, minimum, maximum);
			adjustment = GTK_ADJUSTMENT (gtk_adjustment_new (current_value, minimum, maximum,
								 increment, increment * 10.0, 0.0));
			control = gtk_spin_button_new (adjustment, increment, 0);
			if (ARV_IS_GC_FLOAT (feature))
				digits = (gint) MAX (0, MIN (arv_gc_float_get_display_precision (ARV_GC_FLOAT (feature)), 8));
			gtk_spin_button_set_digits (GTK_SPIN_BUTTON (control), digits);
			gtk_spin_button_set_snap_to_ticks (GTK_SPIN_BUTTON (control), TRUE);
			gtk_entry_set_width_chars (GTK_ENTRY (control), 12);
			data = feature_control_data_new (feature, feature_tree);
			g_signal_connect_data (control, "value-changed", G_CALLBACK (numeric_value_changed_cb), data,
					      feature_control_data_free, 0);
			gtk_widget_set_sensitive (control, editable);
			*out_data = data;
			return control;
		}
	}

	if (ARV_IS_GC_BOOLEAN (feature)) {
		control = gtk_check_button_new ();
		value = get_feature_value (feature);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (control), g_strcmp0 (value, "true") == 0);
		g_free (value);
		data = feature_control_data_new (feature, feature_tree);
		g_signal_connect_data (control, "toggled", G_CALLBACK (boolean_toggled_cb), data,
				      feature_control_data_free, 0);
		gtk_widget_set_sensitive (control, editable);
		*out_data = data;
		return control;
	}

	if (ARV_IS_GC_ENUMERATION (feature)) {
		const char **values;
		const char **display_names;
		guint n_values = 0;
		guint n_display_names = 0;
		guint i;
		const char *current;

		values = arv_gc_enumeration_dup_available_string_values (ARV_GC_ENUMERATION (feature),
									 &n_values, &error);
		if (error != NULL) {
			g_clear_error (&error);
			return gtk_label_new (get_feature_value (feature));
		}
		if (n_values == 0) {
			g_free (values);
			return gtk_label_new (get_feature_value (feature));
		}

		control = gtk_combo_box_text_new ();
		display_names = arv_gc_enumeration_dup_available_display_names (ARV_GC_ENUMERATION (feature),
										&n_display_names, &error);
		if (error != NULL) {
			g_clear_error (&error);
			display_names = NULL;
		}
		for (i = 0; i < n_values; i++)
			gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (control), values[i],
						  display_names != NULL && i < n_display_names ? display_names[i] : values[i]);
		current = arv_gc_enumeration_get_string_value (ARV_GC_ENUMERATION (feature), NULL);
		if (current != NULL)
			gtk_combo_box_set_active_id (GTK_COMBO_BOX (control), current);
		g_free (values);
		g_free (display_names);
		data = feature_control_data_new (feature, feature_tree);
		g_signal_connect_data (control, "changed", G_CALLBACK (enum_changed_cb), data,
				      feature_control_data_free, 0);
		gtk_widget_set_sensitive (control, editable);
		*out_data = data;
		return control;
	}

	if (ARV_IS_GC_STRING (feature) && editable) {
		control = gtk_entry_new ();
		value = get_feature_value (feature);
		gtk_entry_set_text (GTK_ENTRY (control), value);
		g_free (value);
		data = feature_control_data_new (feature, feature_tree);
		g_signal_connect_data (control, "activate", G_CALLBACK (string_entry_activated_cb), data,
				      feature_control_data_free, 0);
		*out_data = data;
		return control;
	}

	return gtk_label_new (get_feature_value (feature));
}

static void
update_feature_control (FeatureRow *row)
{
	ArvGcFeatureNode *feature = row->feature;
	GtkWidget *control = row->control;
	gboolean editable = feature_is_editable (feature);
	char *value;

	if (row->data != NULL)
		row->data->updating = TRUE;

	/* Dispatch on the actual control widget type, not the feature type: an
	 * ArvGcEnumeration is also an ArvGcInteger, so feature type checks would
	 * misclassify enumeration controls. */
	if (GTK_IS_SPIN_BUTTON (control)) {
		double minimum, maximum, increment, current_value;

		value = get_feature_value (feature);
		current_value = g_ascii_strtod (value, NULL);
		g_free (value);
		if (feature_get_numeric_range (feature, &minimum, &maximum, &increment) &&
		    isfinite (current_value)) {
			current_value = CLAMP (current_value, minimum, maximum);
			gtk_spin_button_set_value (GTK_SPIN_BUTTON (control), current_value);
		}
		gtk_widget_set_sensitive (control, editable);
	} else if (GTK_IS_COMBO_BOX (control)) {
		if (ARV_IS_GC_ENUMERATION (feature)) {
			const char *current =
				arv_gc_enumeration_get_string_value (ARV_GC_ENUMERATION (feature), NULL);

			if (current != NULL)
				gtk_combo_box_set_active_id (GTK_COMBO_BOX (control), current);
		}
		gtk_widget_set_sensitive (control, editable);
	} else if (GTK_IS_TOGGLE_BUTTON (control)) {
		value = get_feature_value (feature);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (control),
					      g_strcmp0 (value, "true") == 0);
		g_free (value);
		gtk_widget_set_sensitive (control, editable);
	} else if (GTK_IS_ENTRY (control)) {
		value = get_feature_value (feature);
		gtk_entry_set_text (GTK_ENTRY (control), value);
		g_free (value);
	} else if (GTK_IS_BUTTON (control)) {
		gtk_widget_set_sensitive (control, editable);
	} else if (GTK_IS_LABEL (control)) {
		value = get_feature_value (feature);
		gtk_label_set_text (GTK_LABEL (control), value);
		g_free (value);
	}

	if (row->data != NULL)
		row->data->updating = FALSE;
}

static void
append_feature_row (ArvViewerFeatureTree *feature_tree, guint row_index, ArvGcFeatureNode *feature)
{
	GtkGrid *grid = GTK_GRID (feature_tree->feature_list);
	GtkWidget *name = gtk_label_new (NULL);
	FeatureControlData *data = NULL;
	GtkWidget *value = create_feature_control (feature_tree, feature, &data);
	FeatureRow *row;
	const char *display_name = arv_gc_feature_node_get_display_name (feature);
	const char *unit = NULL;

	if (ARV_IS_GC_INTEGER (feature))
		unit = arv_gc_integer_get_unit (ARV_GC_INTEGER (feature));
	else if (ARV_IS_GC_FLOAT (feature))
		unit = arv_gc_float_get_unit (ARV_GC_FLOAT (feature));

	gtk_label_set_text (GTK_LABEL (name), display_name != NULL ? display_name :
			    arv_gc_feature_node_get_name (feature));
	gtk_label_set_xalign (GTK_LABEL (name), 0.0f);
	gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
	gtk_label_set_width_chars (GTK_LABEL (name), 20);
	gtk_label_set_max_width_chars (GTK_LABEL (name), 32);
	gtk_widget_set_hexpand (name, TRUE);
	gtk_grid_attach (grid, name, 0, row_index, 1, 1);
	gtk_grid_attach (grid, value, 1, row_index, 1, 1);
	if (unit != NULL && (ARV_IS_GC_INTEGER (feature) || ARV_IS_GC_FLOAT (feature))) {
		GtkWidget *unit_label = gtk_label_new (unit);
		gtk_grid_attach (grid, unit_label, 2, row_index, 1, 1);
	}

	row = g_new0 (FeatureRow, 1);
	row->feature = g_object_ref (feature);
	row->control = value;
	row->data = data;
	feature_tree->rows = g_list_append (feature_tree->rows, row);
}

static void
append_category (ArvViewerFeatureTree *feature_tree,
		 ArvGcCategory *category,
		 GtkTreeIter *parent,
		 GHashTable *category_path,
		 guint depth,
		 gboolean root)
{
	GtkTreeIter category_iter;
	GtkTreeIter *child_parent = parent;
	const GSList *features;
	const GSList *iter;
	const char *category_name;
	const char *display_name;

	if (depth > 32)
		return;
	category_name = arv_gc_feature_node_get_name (ARV_GC_FEATURE_NODE (category));
	if (g_hash_table_contains (category_path, category_name))
		return;
	g_hash_table_add (category_path, (gpointer) category_name);

	if (!root) {
		display_name = arv_gc_feature_node_get_display_name (ARV_GC_FEATURE_NODE (category));
		gtk_tree_store_append (feature_tree->group_store, &category_iter, parent);
		gtk_tree_store_set (feature_tree->group_store, &category_iter,
				    GROUP_COLUMN_NAME, display_name != NULL ? display_name : category_name,
				    GROUP_COLUMN_CATEGORY, category,
				    -1);
		child_parent = &category_iter;
	}

	features = arv_gc_category_get_features (category);
	for (iter = features; iter != NULL; iter = iter->next) {
		ArvGcNode *node = arv_gc_get_node (feature_tree->genicam, iter->data);
		ArvGcFeatureNode *feature;

		if (!ARV_IS_GC_FEATURE_NODE (node))
			continue;
		feature = ARV_GC_FEATURE_NODE (node);
		if (arv_gc_feature_node_get_visibility (feature) == ARV_GC_VISIBILITY_INVISIBLE)
			continue;
		if (ARV_IS_GC_CATEGORY (feature))
			append_category (feature_tree, ARV_GC_CATEGORY (feature), child_parent,
					 category_path, depth + 1, FALSE);
	}

	g_hash_table_remove (category_path, category_name);
}

static void
clear_feature_rows (ArvViewerFeatureTree *feature_tree)
{
	GtkWidget *container = feature_tree->feature_list;
	GList *children = gtk_container_get_children (GTK_CONTAINER (container));
	GList *iter;

	for (iter = children; iter != NULL; iter = iter->next)
		gtk_widget_hide (GTK_WIDGET (iter->data));
	for (iter = children; iter != NULL; iter = iter->next)
		gtk_widget_destroy (GTK_WIDGET (iter->data));
	g_list_free (children);

	g_list_free_full (feature_tree->rows, (GDestroyNotify) feature_row_free);
	feature_tree->rows = NULL;
}

static void
refresh_feature_values (ArvViewerFeatureTree *feature_tree)
{
	GList *iter;

	for (iter = feature_tree->rows; iter != NULL; iter = iter->next)
		update_feature_control (iter->data);
}

static void
clear_feature_view (ArvViewerFeatureTree *feature_tree)
{
	gtk_tree_store_clear (feature_tree->group_store);
	clear_feature_rows (feature_tree);
	gtk_label_set_text (GTK_LABEL (feature_tree->group_label), "Select a group");
}

static void
refresh_tree (ArvViewerFeatureTree *feature_tree)
{
	ArvGcNode *root;
	GHashTable *category_path;
	GtkTreeIter first_iter;
	GtkTreePath *first_path;

	clear_feature_view (feature_tree);
	if (feature_tree->genicam == NULL)
		return;

	root = arv_gc_get_node (feature_tree->genicam, "Root");
	if (!ARV_IS_GC_CATEGORY (root))
		return;

	category_path = g_hash_table_new (g_str_hash, g_str_equal);
	append_category (feature_tree, ARV_GC_CATEGORY (root), NULL, category_path, 0, TRUE);
	g_hash_table_unref (category_path);
	if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (feature_tree->group_store), &first_iter)) {
		first_path = gtk_tree_model_get_path (GTK_TREE_MODEL (feature_tree->group_store), &first_iter);
		gtk_tree_view_expand_row (GTK_TREE_VIEW (feature_tree->group_tree), first_path, FALSE);
		gtk_tree_selection_select_iter (
			gtk_tree_view_get_selection (GTK_TREE_VIEW (feature_tree->group_tree)), &first_iter);
		gtk_tree_path_free (first_path);
	}
	gtk_widget_show_all (feature_tree->group_tree);
	gtk_widget_show_all (feature_tree->feature_list);
}

static void
group_selection_changed_cb (GtkTreeSelection *selection, ArvViewerFeatureTree *feature_tree)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	ArvGcCategory *category = NULL;
	const GSList *features;
	const GSList *feature_iter;
	const char *display_name;
	const char *category_name;
	guint row_index = 0;

	clear_feature_rows (feature_tree);
	if (feature_tree->genicam == NULL) {
		gtk_label_set_text (GTK_LABEL (feature_tree->group_label), "Select a group");
		return;
	}
	if (!gtk_tree_selection_get_selected (selection, &model, &iter)) {
		gtk_label_set_text (GTK_LABEL (feature_tree->group_label), "Select a group");
		return;
	}

	gtk_tree_model_get (model, &iter, GROUP_COLUMN_CATEGORY, &category, -1);
	if (!ARV_IS_GC_CATEGORY (category)) {
		g_clear_object (&category);
		return;
	}

	display_name = arv_gc_feature_node_get_display_name (ARV_GC_FEATURE_NODE (category));
	category_name = arv_gc_feature_node_get_name (ARV_GC_FEATURE_NODE (category));
	gtk_label_set_text (GTK_LABEL (feature_tree->group_label),
			    display_name != NULL ? display_name : category_name);
	features = arv_gc_category_get_features (category);
	for (feature_iter = features; feature_iter != NULL; feature_iter = feature_iter->next) {
		ArvGcNode *node = arv_gc_get_node (feature_tree->genicam, feature_iter->data);
		ArvGcFeatureNode *feature;

		if (!ARV_IS_GC_FEATURE_NODE (node))
			continue;
		feature = ARV_GC_FEATURE_NODE (node);
		if (ARV_IS_GC_CATEGORY (feature) ||
		    arv_gc_feature_node_get_visibility (feature) == ARV_GC_VISIBILITY_INVISIBLE)
			continue;
		append_feature_row (feature_tree, row_index++, feature);
	}
	gtk_widget_show_all (feature_tree->feature_list);
	g_object_unref (category);
}

static void
refresh_button_clicked_cb (GtkButton *button, ArvViewerFeatureTree *feature_tree)
{
	(void) button;
	arv_viewer_feature_tree_refresh (feature_tree);
}

static gboolean
auto_refresh_timeout_cb (gpointer user_data)
{
	ArvViewerFeatureTree *feature_tree = user_data;

	arv_viewer_feature_tree_refresh (feature_tree);

	return TRUE;
}

static void
auto_refresh_toggled_cb (GtkToggleToolButton *button, ArvViewerFeatureTree *feature_tree)
{
	if (gtk_toggle_tool_button_get_active (button)) {
		if (feature_tree->auto_refresh_event == 0)
			feature_tree->auto_refresh_event =
				g_timeout_add_seconds (2, auto_refresh_timeout_cb, feature_tree);
	} else if (feature_tree->auto_refresh_event > 0) {
		g_source_remove (feature_tree->auto_refresh_event);
		feature_tree->auto_refresh_event = 0;
	}
}

static void
arv_viewer_feature_tree_init (ArvViewerFeatureTree *feature_tree)
{
	gtk_orientable_set_orientation (GTK_ORIENTABLE (feature_tree), GTK_ORIENTATION_VERTICAL);
	feature_tree->group_store = gtk_tree_store_new (N_GROUP_COLUMNS,
						       G_TYPE_STRING,
						       ARV_TYPE_GC_CATEGORY);
}

static void
arv_viewer_feature_tree_adopt_widgets (ArvViewerFeatureTree *feature_tree, GtkBuilder *builder)
{
	GtkWidget *refresh_button;
	GtkWidget *feature_scrolled_window;

	feature_tree->container = GTK_WIDGET (gtk_builder_get_object (builder, "feature_tree_box"));
	feature_tree->group_tree = GTK_WIDGET (gtk_builder_get_object (builder, "feature_group_tree"));
	feature_tree->group_label = GTK_WIDGET (gtk_builder_get_object (builder, "feature_group_label"));
	gtk_tree_view_set_model (GTK_TREE_VIEW (feature_tree->group_tree),
				 GTK_TREE_MODEL (feature_tree->group_store));
	g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (feature_tree->group_tree)),
			  "changed", G_CALLBACK (group_selection_changed_cb), feature_tree);

	/* The feature rows are created dynamically, so the grid holding them is not
	 * described in the UI file. Reuse a builder-provided grid if there is one,
	 * otherwise create it and pack it into the UI-provided scrolled window. */
	feature_tree->feature_list = GTK_WIDGET (gtk_builder_get_object (builder, "feature_grid"));
	if (feature_tree->feature_list == NULL) {
		feature_tree->feature_list = gtk_grid_new ();
		gtk_grid_set_row_spacing (GTK_GRID (feature_tree->feature_list), 2);
		gtk_grid_set_column_spacing (GTK_GRID (feature_tree->feature_list), 10);
		gtk_grid_set_column_homogeneous (GTK_GRID (feature_tree->feature_list), FALSE);
	}
	feature_scrolled_window = GTK_WIDGET (gtk_builder_get_object (builder, "feature_scrolled_window"));
	if (feature_scrolled_window != NULL &&
	    gtk_widget_get_parent (feature_tree->feature_list) == NULL)
		gtk_container_add (GTK_CONTAINER (feature_scrolled_window), feature_tree->feature_list);
	gtk_widget_show (feature_tree->feature_list);

	refresh_button = GTK_WIDGET (gtk_builder_get_object (builder, "feature_refresh_button"));
	g_signal_connect (refresh_button, "clicked", G_CALLBACK (refresh_button_clicked_cb), feature_tree);

	feature_tree->auto_refresh_button =
		GTK_WIDGET (gtk_builder_get_object (builder, "feature_auto_refresh_button"));
	if (feature_tree->auto_refresh_button != NULL)
		g_signal_connect (feature_tree->auto_refresh_button, "toggled",
				  G_CALLBACK (auto_refresh_toggled_cb), feature_tree);

	/* No camera selected yet: keep the browser hidden until a device is set. */
	gtk_widget_set_visible (feature_tree->container, FALSE);
}

static void
arv_viewer_feature_tree_dispose (GObject *object)
{
	ArvViewerFeatureTree *feature_tree = ARV_VIEWER_FEATURE_TREE (object);

	if (feature_tree->auto_refresh_event > 0) {
		g_source_remove (feature_tree->auto_refresh_event);
		feature_tree->auto_refresh_event = 0;
	}
	if (feature_tree->genicam != NULL)
		clear_feature_view (feature_tree);
	g_list_free_full (feature_tree->rows, (GDestroyNotify) feature_row_free);
	feature_tree->rows = NULL;
	g_clear_object (&feature_tree->genicam);
	g_clear_object (&feature_tree->group_store);
	G_OBJECT_CLASS (arv_viewer_feature_tree_parent_class)->dispose (object);
}

static void
arv_viewer_feature_tree_class_init (ArvViewerFeatureTreeClass *class)
{
	G_OBJECT_CLASS (class)->dispose = arv_viewer_feature_tree_dispose;
}

GtkWidget *
arv_viewer_feature_tree_new (GtkBuilder *builder)
{
	ArvViewerFeatureTree *feature_tree;

	g_return_val_if_fail (GTK_IS_BUILDER (builder), NULL);

	feature_tree = g_object_new (ARV_TYPE_VIEWER_FEATURE_TREE, NULL);
	arv_viewer_feature_tree_adopt_widgets (feature_tree, builder);

	return GTK_WIDGET (feature_tree);
}

void
arv_viewer_feature_tree_set_device (ArvViewerFeatureTree *feature_tree, ArvDevice *device)
{
	g_return_if_fail (ARV_IS_VIEWER_FEATURE_TREE (feature_tree));
	g_return_if_fail (device == NULL || ARV_IS_DEVICE (device));

	if (device == NULL) {
		if (feature_tree->genicam != NULL)
			clear_feature_view (feature_tree);
		g_clear_object (&feature_tree->genicam);
		if (feature_tree->container != NULL)
			gtk_widget_set_visible (feature_tree->container, FALSE);
		return;
	}

	g_set_object (&feature_tree->genicam, device != NULL ? arv_device_get_genicam (device) : NULL);
	refresh_tree (feature_tree);
	if (feature_tree->container != NULL)
		gtk_widget_set_visible (feature_tree->container, TRUE);
}

void
arv_viewer_feature_tree_set_change_guard (ArvViewerFeatureTree *feature_tree,
					  ArvViewerFeatureTreeChangeBeginFunc begin,
					  ArvViewerFeatureTreeChangeEndFunc end,
					  gpointer user_data)
{
	g_return_if_fail (ARV_IS_VIEWER_FEATURE_TREE (feature_tree));

	feature_tree->change_begin = begin;
	feature_tree->change_end = end;
	feature_tree->change_user_data = user_data;
}

void
arv_viewer_feature_tree_set_refresh_func (ArvViewerFeatureTree *feature_tree,
					  ArvViewerFeatureTreeRefreshFunc refresh,
					  gpointer user_data)
{
	g_return_if_fail (ARV_IS_VIEWER_FEATURE_TREE (feature_tree));

	feature_tree->refresh_func = refresh;
	feature_tree->refresh_user_data = user_data;
}

void
arv_viewer_feature_tree_refresh (ArvViewerFeatureTree *feature_tree)
{
	g_return_if_fail (ARV_IS_VIEWER_FEATURE_TREE (feature_tree));

	refresh_feature_values (feature_tree);

	if (feature_tree->refresh_func != NULL)
		feature_tree->refresh_func (feature_tree->refresh_user_data);
}