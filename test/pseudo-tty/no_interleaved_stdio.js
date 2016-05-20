// https://github.com/nodejs/node/issues/6456#issuecomment-219320599
// https://gist.github.com/isaacs/1495b91ec66b21d30b10572d72ad2cdd
'use strict';
require('../common');

// 1000 bytes wrapped at 50 columns
// Array(num).join() "loops" one less time than may be expected.
// \n turns into a double-byte character
// 20 * (48 + (2))
var out = new Array(21).join(new Array(49).join('o') + '\n');
// Add the remaining 21 bytes and terminate with an 'O\n'.
// This results in 1025 bytes, just enough to overflow the 1kb OS X TTY buffer.
out += new Array(25).join('o') + 'O';

const err = '__This is some stderr__';

process.stdout.write(out);
process.stderr.write(err);
