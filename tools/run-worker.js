'use strict';
const path = require('path');
const { Worker } = require('worker');

new Worker(path.resolve(process.cwd(), process.argv[2]));
