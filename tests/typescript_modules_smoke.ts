import { greet, tag } from './typescript_modules_aux';

interface FxeImportMeta {
  url: string;
  filename: string;
  dirname: string;
  main: boolean;
}

const meta = import.meta as unknown as FxeImportMeta;
const has_filename: boolean = typeof meta.filename === 'string' && meta.filename.length > 0;
const is_main: boolean = meta.main === true;

console.log(`ts-modules=${greet(tag)}|main=${is_main}|file=${has_filename}`);
