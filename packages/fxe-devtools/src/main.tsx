/** @jsxImportSource fxe-ui */
import { App, Window } from 'fxe';
import { mount } from 'fxe-ui';
import { Shell } from './Shell.tsx';

const port = globalThis.FXE_DEBUG_PORT ?? 9333;
const url = `ws://127.0.0.1:${port}/devtools/page/fxe-main`;
const win = new Window({ title: 'FXE DevTools', width: 900, height: 600 });

mount(<Shell url={url} />, win);
App.run({ animate: true });
