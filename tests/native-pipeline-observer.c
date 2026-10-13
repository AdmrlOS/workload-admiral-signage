#include "../native/gst-admiral-holepunch.c"
#include <assert.h>
int main(int argc, char **argv) {
 gst_init(&argc,&argv);
 AdmiralSink *s=g_object_new(admiral_sink_get_type(),NULL);
 gst_object_ref_sink(s);
 watch_pipeline(s);
 GstElement *p=gst_pipeline_new("actual-webkit-pipeline");
 gst_bin_add(GST_BIN(p),GST_ELEMENT(gst_object_ref(s)));
 watch_pipeline(s);
 assert(s->pipeline==p);
 gulong handler=s->element_handler;
 watch_pipeline(s);
 assert(s->pipeline==p && s->element_handler==handler);
 release_resources(s);
 gst_object_unref(p);gst_object_unref(s);
 puts("late pipeline attachment PASS");
 return 0;
}
