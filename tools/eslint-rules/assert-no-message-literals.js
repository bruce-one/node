/**
 * @fileoverview Check that calls to assert methods never have a simple
 * string literal as the `message` argument.
 * @author Anna Henningsen
 */
'use strict';

//------------------------------------------------------------------------------
// Rule Definition
//------------------------------------------------------------------------------

function checkThrowsArguments(context, node) {
  if (node.callee.type === 'MemberExpression' &&
      node.callee.object.name === 'assert') {
    const args = node.arguments;
    let messageArg = null;
    switch (node.callee.property.name) {
      case 'deepEqual':
      case 'deepStrictEqual':
      case 'equal':
      case 'notDeepEqual':
      case 'notDeepStrictEqual':
      case 'notEqual':
      case 'notStrictEqual':
      case 'strictEqual':
        if (args.length > 2)
          messageArg = args[2];
        break;
      case 'ok':
        if (args.length > 1)
          messageArg = args[1];
        break;
      case 'doesNotThrow':
      case 'throws':
        if (args.length > 2) {
          messageArg = args[2];
        } else if (args.length > 1 && args[1].type === 'Literal' &&
                   !args[1].regex) {
          messageArg = args[1];
        }
        break;
      case 'ifError':
      case 'fail':
        return;
    }

    if (messageArg === null)
      return;

    if (messageArg.type === 'Literal') {
      context.report({
        message: 'Unexpected literal assertion message',
        node: messageArg
      });
    }
  }
}

module.exports = {
  meta: {
    schema: [
      {
        type: 'object',
      }
    ]
  },
  create: function(context) {
    return {
      CallExpression: (node) => checkThrowsArguments(context, node)
    };
  }
};
