import assert from 'node:assert/strict';
import {
  normalizeRecentExpertIds,
  recordRecentExpert,
  resolveRecentExperts,
  validateRecentExpertIds,
} from './recentExperts.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('recent experts normalize to five unique non-empty IDs', () => {
  assert.deepEqual(
    normalizeRecentExpertIds([' a ', 'b', '', 'a', 3, 'c', 'd', 'e', 'f']),
    ['a', 'b', 'c', 'd', 'e'],
  );
  assert.equal(validateRecentExpertIds(['a', 'b', 'c', 'd', 'e']), true);
  assert.equal(validateRecentExpertIds(['a', 'a']), false);
  assert.equal(validateRecentExpertIds(['a', 'b', 'c', 'd', 'e', 'f']), false);
  assert.deepEqual(normalizeRecentExpertIds(['a'], 0), []);
});

test('recording an expert moves it to the front and keeps the shared five-item limit', () => {
  assert.deepEqual(
    recordRecentExpert(['team-a', 'expert-b', 'expert-c', 'expert-d', 'expert-e'], 'expert-c'),
    ['expert-c', 'team-a', 'expert-b', 'expert-d', 'expert-e'],
  );
  assert.deepEqual(
    recordRecentExpert(['team-a', 'expert-b', 'expert-c', 'expert-d', 'expert-e'], 'team-f'),
    ['team-f', 'team-a', 'expert-b', 'expert-c', 'expert-d'],
  );
});

test('recent expert resolution preserves recency and omits unavailable components', () => {
  const experts = [
    { id: 'expert-b', type: 'agent' },
    { id: 'team-a', type: 'team' },
    { id: 'expert-c', type: 'agent' },
  ];
  assert.deepEqual(
    resolveRecentExperts(experts, ['team-a', 'missing', 'expert-c', 'expert-b'])
      .map((expert) => expert.id),
    ['team-a', 'expert-c', 'expert-b'],
  );
});
