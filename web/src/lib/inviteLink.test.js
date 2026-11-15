import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { decodeInviteLink, extractInviteLink } from './inviteLink.js';

const fixtures = new URL('../../../tests/fixtures/invite_links/', import.meta.url);
for (const name of readdirSync(fixtures)) {
  const fixture = JSON.parse(readFileSync(new URL(name, fixtures), 'utf8'));
  const result = decodeInviteLink(fixture.text, fixture.now);
  assert.equal(result.error || '', fixture.error || '', name);
  if (fixture.value) assert.deepEqual(result.value, fixture.value, name);
}
assert.equal(extractInviteLink('ordinary clipboard text'), '');
console.log('inviteLink tests passed');
