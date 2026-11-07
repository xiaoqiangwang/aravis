#ifndef ARV_FEATURE_TREE_H
#define ARV_FEATURE_TREE_H

#include <gtk/gtk.h>
#include <arv.h>

G_BEGIN_DECLS

#define ARV_TYPE_VIEWER_FEATURE_TREE (arv_viewer_feature_tree_get_type ())
G_DECLARE_FINAL_TYPE (ArvViewerFeatureTree, arv_viewer_feature_tree, ARV, VIEWER_FEATURE_TREE, GtkBox)

/* Called before a feature is written. Returns an opaque state token that is
 * handed back to the end function, so the caller can restore the previous
 * camera state (e.g. restart acquisition if it was running). */
typedef gpointer (*ArvViewerFeatureTreeChangeBeginFunc)	(gpointer user_data);
typedef void	 (*ArvViewerFeatureTreeChangeEndFunc)	(gpointer user_data, gpointer state);

/* Called when the user asks for a refresh, so external widgets (e.g. the
 * basic features tab) can update themselves too. */
typedef void	 (*ArvViewerFeatureTreeRefreshFunc)	(gpointer user_data);

GtkWidget *	arv_viewer_feature_tree_new		(GtkBuilder *builder);
void		arv_viewer_feature_tree_set_device	(ArvViewerFeatureTree *feature_tree,
							 ArvDevice *device);
void		arv_viewer_feature_tree_set_change_guard (ArvViewerFeatureTree *feature_tree,
							  ArvViewerFeatureTreeChangeBeginFunc begin,
							  ArvViewerFeatureTreeChangeEndFunc end,
							  gpointer user_data);
void		arv_viewer_feature_tree_set_refresh_func (ArvViewerFeatureTree *feature_tree,
							  ArvViewerFeatureTreeRefreshFunc refresh,
							  gpointer user_data);
void		arv_viewer_feature_tree_refresh		(ArvViewerFeatureTree *feature_tree);

G_END_DECLS

#endif