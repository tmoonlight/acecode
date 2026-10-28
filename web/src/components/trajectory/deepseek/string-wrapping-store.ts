const KEY = 'ace.trajectory.jsonStringWrapping'
let fallback = false

/** Sample on expansion, so other open strings keep their own wrapping state. */
export const trajectoryStringWrapping = {
  label: 'Wrap lines',
  getDefault(): boolean {
    try {
      const value = globalThis.localStorage?.getItem(KEY)
      return value === null || value === undefined ? fallback : value === 'true'
    } catch {
      return fallback
    }
  },
  setDefault(wrapped: boolean) {
    fallback = wrapped
    try { globalThis.localStorage?.setItem(KEY, String(wrapped)) } catch { /* Optional persistence. */ }
  },
}
