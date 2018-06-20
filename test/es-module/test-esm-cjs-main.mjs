// Flags: --experimental-modules
import '../common';
import assert from 'assert';
import fixtures from '../common/fixtures';
import { spawnSync } from 'child_process';

const { stdout } = spawnSync(process.execPath, [
  '--experimental-modules', fixtures.path('printA.js')
], { encoding: 'utf8' });
assert.strictEqual(stdout, 'A\n');
