import { useEffect, useState } from 'react';
import { transport } from '../transport';
import { useManifestStore } from '../store/manifest';

export function useRuntimeCapabilities() {
  const coreInfo = useManifestStore((state) => state.coreInfo);
  const [capabilities, setCapabilities] = useState<{
    resultBindings?: boolean;
    cloudSampling?: 'ordered-stride-v1';
  }>({});
  useEffect(() => {
    let cancelled = false;
    setCapabilities({});
    void transport
      .getRuntimeCapabilities?.()
      .then((value) => {
        if (!cancelled) setCapabilities(value);
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, [coreInfo]);
  return capabilities;
}
