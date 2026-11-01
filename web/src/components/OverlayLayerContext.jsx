import { createContext, useContext } from 'react';

// Portalled menus/dialogs inherit the surface that owns them.
export const OverlayLayerContext = createContext(undefined);
export const useOverlayLayer = () => useContext(OverlayLayerContext);
