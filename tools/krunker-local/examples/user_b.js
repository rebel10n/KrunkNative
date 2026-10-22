// Sample user script B: runs after A (the order given) and checks A's flag. Sets __userB to true when A ran first.
window.__userB = window.__userA === true;
