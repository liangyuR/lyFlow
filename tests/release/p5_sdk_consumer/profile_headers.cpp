#include "lyflow/hash.h"
#include "lyflow/graph_migration.h"
#include "lyflow/profile.h"

lyflow::ProfileDescriptor profileHeaderContract() {
  lyflow::ProfileDescriptor descriptor;
  descriptor.frame = "sensor";
  descriptor.unit = "mm";
  descriptor.plane = "XZ";
  descriptor.handedness = "right";
  descriptor.sampleOrder = "acquisition";
  descriptor.sourceId = "sdk-header";
  return descriptor;
}
