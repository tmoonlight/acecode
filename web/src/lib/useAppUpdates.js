import { useEffect, useMemo, useSyncExternalStore } from 'react';
import { api, apiConnectionScope } from './api.js';
import { createUpdateController, automaticUpdateFailed } from './automaticUpdates.js';
import { UPGRADE_CONFIG_CHANGED_EVENT } from './themeImports.js';
import { toast } from '../components/Toast.jsx';

export function useAppUpdates(authState) {
  const scope = apiConnectionScope(api);
  const controller = useMemo(() => createUpdateController({
    api, notify: toast, isCurrentConnection: () => apiConnectionScope(api) === scope,
  }), [scope]);
  const state = useSyncExternalStore(controller.subscribe, controller.getSnapshot);
  useEffect(() => {
    if (authState !== 'ok') return undefined;
    void controller.start();
    const changed = () => controller.refreshConfiguration();
    window.addEventListener(UPGRADE_CONFIG_CHANGED_EVENT, changed);
    return () => {
      window.removeEventListener(UPGRADE_CONFIG_CHANGED_EVENT, changed);
      controller.stop();
    };
  }, [authState, controller]);
  return {
    updateStatus: state.status,
    updateJob: state.job,
    updateStarting: state.starting,
    updateChecking: state.checking,
    updateDialogOpen: state.dialogOpen,
    updateFailed: automaticUpdateFailed(state.status, state.job),
    setUpdateJob: controller.setJob,
    setUpdateDialogOpen: controller.setDialogOpen,
    openUpdateDialog: controller.openDialog,
    checkForUpdates: () => controller.check({ manual: true }),
    startUpdate: () => controller.startUpdate(),
  };
}
