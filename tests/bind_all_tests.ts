import { run } from './ts_harness.ts';

import './bind_harness_test.ts';

// === bind test imports begin ===
import './bind_window_test.ts';
import './bind_renderer_test.ts';
import './bind_command_buffer_test.ts';
import './bind_primitives_test.ts';
import './bind_fs_test.ts';
import './bind_path_test.ts';
import './bind_timers_test.ts';
import './bind_process_test.ts';
import './bind_fetch_test.ts';
import './bind_url_test.ts';
import './bind_websocket_test.ts';
import './bind_image_test.ts';
import './bind_spritesheet_test.ts';
import './bind_font_test.ts';
import './bind_audio_test.ts';
import './bind_app_test.ts';
import './bind_shell_test.ts';
import './bind_dialog_test.ts';
import './bind_notification_test.ts';
import './bind_menu_test.ts';
import './bind_tray_test.ts';
import './bind_global_shortcut_test.ts';
import './bind_performance_test.ts';
import './bind_render_stats_test.ts';
import './bind_sqlite_test.ts';
import './bind_storage_test.ts';
// === bind test imports end ===

await run();
