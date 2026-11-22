import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { parseSync } from '@babel/core';
import { transformWithEsbuild } from 'vite';
import { createPageAnnotationController } from './pageAnnotations.js';

const source = fs.readFileSync(new URL('../components/PageAnnotationTextField.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, {configFile:false, babelrc:false, parserOpts:{plugins:['jsx']}});
const body = ast.program.body.filter(node => node.type !== 'ImportDeclaration').map(node => node.declaration || node);
const transformed = await transformWithEsbuild(body.map(node => source.slice(node.start, node.end)).join('\n'),
  'PageAnnotationTextField.jsx', {loader:'jsx', jsxFactory:'React.createElement'});
let value;
let dependencies;
const useState = initial => [value ??= initial, next => { value = next; }];
const useEffect = (effect, next) => {
  if (!dependencies || next.some((item, index) => item !== dependencies[index])) effect();
  dependencies = next;
};
const {PageAnnotationTextField} = vm.runInNewContext(`${transformed.code}; ({PageAnnotationTextField});`,
  {React, useEffect, useState});
const textarea = field => field.props.children.find(child => child?.type === 'textarea');

{
  const requests = [];
  let release;
  let revision = 1;
  const target = {id:'target-1', text_preview:{value:'Original', editable:true, error:''}};
  const controller = createPageAnnotationController({pageId:'page-1', invoke:async (pageId, command) => {
    requests.push(command);
    if (requests.length === 1) await new Promise(resolve => { release = resolve; });
    return {ok:true, page_id:pageId, annotations:{document_id:'doc', target, revision:++revision}};
  }});
  controller.observe({document_id:'doc', target, revision});
  const promises = [];
  const props = {target, working:false, command:(...args) => {
    const promise = controller.command(...args);
    promises.push(promise);
    return promise;
  }};
  let field = PageAnnotationTextField(props);
  assert.equal(textarea(field).props['aria-label'], '可见文本');
  textarea(field).props.onChange({target:{value:'First'}});
  field = PageAnnotationTextField(props);
  textarea(field).props.onChange({target:{value:'Latest'}});
  field = PageAnnotationTextField({...props, target:{...target, text_preview:{...target.text_preview, value:'First'}}});
  assert.equal(textarea(field).props.value, 'Latest');
  const save = controller.command('save', {description:''});
  await Promise.resolve();
  assert.equal(requests.length, 1);
  release();
  await Promise.all([...promises, save]);
  assert.deepEqual(requests.map(command => command.action), ['preview_text', 'preview_text', 'save']);
  assert.deepEqual(requests.slice(0, 2).map(command => command.text), ['First', 'Latest']);
  assert.equal(requests[2].target_id, 'target-1');
  assert.ok(requests.every(command => command.document_id === 'doc'));
  field = PageAnnotationTextField({...props, target:{...target, id:'target-2', text_preview:{value:'Second target', editable:true, error:''}}});
  field = PageAnnotationTextField({...props, target:{...target, id:'target-2', text_preview:{value:'Second target', editable:true, error:''}}});
  assert.equal(textarea(field).props.value, 'Second target');
  field = PageAnnotationTextField({...props, working:true});
  assert.equal(textarea(field).props.disabled, true);
  field = PageAnnotationTextField({...props, target:{...target, text_preview:{value:'', editable:false, error:'Text is not editable'}}});
  assert.equal(textarea(field).props.disabled, true);
  assert.equal(field.props.children[2].props.role, 'alert');
  assert.equal(field.props.children[2].props.children, 'Text is not editable');
  // Native prose hints must be localized through the i18n catalog, never shown raw.
  field = PageAnnotationTextField({...props, target:{...target, text_preview:{value:'', editable:false, error:'Text exceeds the editable limit or is not visible content'}}});
  assert.equal(field.props.children[2].props.children, '文本超出可编辑上限，或该内容不可见');
  field = PageAnnotationTextField({...props, target:{...target, text_preview:{value:'', editable:false, error:'Text nodes changed; cancel and select the element again'}}});
  assert.equal(field.props.children[2].props.children, '文本节点已变化，请取消后重新选择元素');
  console.log('[pass] production text field keeps rapid local edits, binds target/document and queues save after previews');
}
