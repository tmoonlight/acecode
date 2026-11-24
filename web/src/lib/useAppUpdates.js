import { useEffect, useMemo, useSyncExternalStore } from 'react';
import { api, apiConnectionScope } from './api.js';
import { createUpdateController, automaticUpdateFailed } from './automaticUpdates.js';
import { UPGRADE_CONFIG_CHANGED_EVENT } from './themeImports.js';
import { upgradeSettingsStore } from './upgradeSettings.js';
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
    updateStartingRollback: state.startingRollback,
    updateChecking: state.checking,
    updateDialogOpen: state.dialogOpen,
    updateFailed: automaticUpdateFailed(state.status, state.job),
    setUpdateJob: controller.setJob,
    setUpdateDialogOpen: controller.setDialogOpen,
    openUpdateDialog: controller.openDialog,
    checkForUpdates: () => controller.check({ manual: true }),
    startUpdate: () => controller.startUpdate(),
    // daemon 在回滚请求里关掉了自动更新:结束后重读设置,弹窗开关与设置页同步。
    startRollback: (version) => controller.startRollback(version)
      .finally(() => { void upgradeSettingsStore(api).load(); }),
  };
}
