#!/bin/bash
# Integration tests for ElCache HTTP API
# Requires running ElCache server
#
# Usage: ./test_http_api.sh [host:port]
# Default: localhost:8080

HOST="${1:-localhost:8080}"
BASE_URL="http://${HOST}"
PASS=0
FAIL=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

pass() {
    echo -e "${GREEN}PASS${NC}: $1"
    PASS=$((PASS + 1))
}

fail() {
    echo -e "${RED}FAIL${NC}: $1 - $2"
    FAIL=$((FAIL + 1))
}

echo "ElCache HTTP API Tests"
echo "======================"
echo "Target: ${BASE_URL}"
echo ""

# Test 1: Health check
echo -n "Test 1: Health check... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/health")
if [ "$STATUS" = "200" ]; then
    pass "Health check"
else
    fail "Health check" "Expected 200, got $STATUS"
fi

# Test 2: PUT small value
echo -n "Test 2: PUT small value... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PUT "${BASE_URL}/cache/test_small" -d "Hello World")
if [ "$STATUS" = "201" ]; then
    pass "PUT small value"
else
    fail "PUT small value" "Expected 201, got $STATUS"
fi

# Test 3: GET small value
echo -n "Test 3: GET small value... "
BODY=$(curl -s "${BASE_URL}/cache/test_small")
if [ "$BODY" = "Hello World" ]; then
    pass "GET small value"
else
    fail "GET small value" "Expected 'Hello World', got '$BODY'"
fi

# Test 4: PUT larger value (10KB)
echo -n "Test 4: PUT 10KB value... "
dd if=/dev/urandom bs=10240 count=1 2>/dev/null | base64 > /tmp/test_10k.dat
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PUT "${BASE_URL}/cache/test_10k" --data-binary @/tmp/test_10k.dat)
if [ "$STATUS" = "201" ]; then
    pass "PUT 10KB value"
else
    fail "PUT 10KB value" "Expected 201, got $STATUS"
fi

# Test 5: GET 10KB value and verify size
echo -n "Test 5: GET 10KB value... "
curl -s "${BASE_URL}/cache/test_10k" > /tmp/test_10k_get.dat
ORIG_SIZE=$(wc -c < /tmp/test_10k.dat)
GET_SIZE=$(wc -c < /tmp/test_10k_get.dat)
if [ "$ORIG_SIZE" = "$GET_SIZE" ]; then
    pass "GET 10KB value (size: $GET_SIZE bytes)"
else
    fail "GET 10KB value" "Size mismatch: original=$ORIG_SIZE, got=$GET_SIZE"
fi

# Test 6: PUT 100KB value
echo -n "Test 6: PUT 100KB value... "
dd if=/dev/urandom bs=102400 count=1 2>/dev/null | base64 > /tmp/test_100k.dat
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PUT "${BASE_URL}/cache/test_100k" --data-binary @/tmp/test_100k.dat)
if [ "$STATUS" = "201" ]; then
    pass "PUT 100KB value"
else
    fail "PUT 100KB value" "Expected 201, got $STATUS"
fi

# Test 7: HEAD request for metadata
echo -n "Test 7: HEAD request... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -I "${BASE_URL}/cache/test_100k")
if [ "$STATUS" = "200" ]; then
    pass "HEAD request"
else
    fail "HEAD request" "Expected 200, got $STATUS"
fi

# Test 8: Range request
echo -n "Test 8: Range request... "
RANGE_DATA=$(curl -s -H "Range: bytes=0-9" "${BASE_URL}/cache/test_small")
if [ "$RANGE_DATA" = "Hello Worl" ]; then
    pass "Range request"
else
    fail "Range request" "Expected 'Hello Worl', got '$RANGE_DATA'"
fi

# Test 9: DELETE
echo -n "Test 9: DELETE... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "${BASE_URL}/cache/test_small")
if [ "$STATUS" = "204" ]; then
    pass "DELETE"
else
    fail "DELETE" "Expected 204, got $STATUS"
fi

# Test 10: GET after DELETE should 404
echo -n "Test 10: GET after DELETE... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/cache/test_small")
if [ "$STATUS" = "404" ]; then
    pass "GET after DELETE returns 404"
else
    fail "GET after DELETE" "Expected 404, got $STATUS"
fi

# Test 11: Stats endpoint
echo -n "Test 11: Stats endpoint... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/stats")
if [ "$STATUS" = "200" ]; then
    pass "Stats endpoint"
else
    fail "Stats endpoint" "Expected 200, got $STATUS"
fi

# Test 12: PUT with TTL header
echo -n "Test 12: PUT with TTL... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PUT "${BASE_URL}/cache/test_ttl" -H "X-ElCache-TTL: 3600" -d "TTL Test")
if [ "$STATUS" = "201" ]; then
    pass "PUT with TTL"
else
    fail "PUT with TTL" "Expected 201, got $STATUS"
fi

# Test 13: Sparse write - Create
echo -n "Test 13: Sparse write - Create... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${BASE_URL}/sparse/test_sparse" -H "X-ElCache-Size: 1024")
if [ "$STATUS" = "201" ]; then
    pass "Sparse create"
else
    fail "Sparse create" "Expected 201, got $STATUS"
fi

# Test 14: Sparse write - Write range
echo -n "Test 14: Sparse write - Write range... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PATCH "${BASE_URL}/sparse/test_sparse?offset=0" -d "$(head -c 512 /dev/zero | tr '\0' 'A')")
if [ "$STATUS" = "202" ]; then
    pass "Sparse write range 1"
else
    fail "Sparse write range 1" "Expected 202, got $STATUS"
fi

STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PATCH "${BASE_URL}/sparse/test_sparse?offset=512" -d "$(head -c 512 /dev/zero | tr '\0' 'B')")
if [ "$STATUS" = "202" ]; then
    pass "Sparse write range 2"
else
    fail "Sparse write range 2" "Expected 202, got $STATUS"
fi

# Test 15: Sparse write - Status check
echo -n "Test 15: Sparse write - Status... "
COMPLETION=$(curl -s "${BASE_URL}/sparse/test_sparse" | grep -o '"completion_percent": [0-9]*' | grep -o '[0-9]*')
if [ "$COMPLETION" = "100" ]; then
    pass "Sparse status shows 100%"
else
    fail "Sparse status" "Expected 100% completion, got $COMPLETION%"
fi

# Test 16: Sparse write - Finalize
echo -n "Test 16: Sparse write - Finalize... "
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${BASE_URL}/sparse/test_sparse/finalize")
if [ "$STATUS" = "201" ]; then
    pass "Sparse finalize"
else
    fail "Sparse finalize" "Expected 201, got $STATUS"
fi

# Test 17: Read finalized sparse data
echo -n "Test 17: Read finalized sparse... "
DATA=$(curl -s -H "Range: bytes=0-0" "${BASE_URL}/cache/test_sparse")
if [ "$DATA" = "A" ]; then
    pass "Read finalized sparse (first byte)"
else
    fail "Read finalized sparse" "Expected 'A', got '$DATA'"
fi

DATA=$(curl -s -H "Range: bytes=512-512" "${BASE_URL}/cache/test_sparse")
if [ "$DATA" = "B" ]; then
    pass "Read finalized sparse (byte 512)"
else
    fail "Read finalized sparse" "Expected 'B', got '$DATA'"
fi

# Cleanup
rm -f /tmp/test_10k.dat /tmp/test_10k_get.dat /tmp/test_100k.dat

echo ""
echo "======================"
echo "Results: ${PASS} passed, ${FAIL} failed"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
