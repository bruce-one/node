'use strict';

const fs = require('fs');
const util = require('util');
const async_hooks = require('async_hooks');

// async_hooks debug dump code...
function debug(...args) {
  fs.writeFileSync('/dev/stderr', `${util.format(...args)}\n`, { flag: 'a' });
}

async_hooks.createHook({
  init(id, type, triggerId, resoure) { debug('init', { id, type, triggerId, resoure }); },
  before(id) { debug('before', { id }); },
  after(id) { debug('after', { id }); },
  destroy(id) { debug('destroy', { id }); },
}).enable();
// ...up until here


const { ruhrJsTimeout } = require('timers');

ruhrJsTimeout(() => debug('hi'), 100000);
