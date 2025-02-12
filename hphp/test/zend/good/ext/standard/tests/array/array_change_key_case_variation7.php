<?php
/* Prototype  : array array_change_key_case(array $input [, int $case])
 * Description: Retuns an array with all string keys lowercased [or uppercased] 
 * Source code: ext/standard/array.c
 */

/*
 * Test array_change_key_case() when:
 * 1. Passed a referenced variable
 * 2. Passed an argument by reference
 */

echo "*** Testing array_change_key_case() : usage variations ***\n";

$input = array('one' => 1, 'two' => 2, 'ABC' => 'xyz');

echo "\n-- \$input argument is a reference to array --\n";
$new_input = &$input;
echo "Result:\n";
var_dump(array_change_key_case($new_input, CASE_UPPER));
echo "Original:\n";
var_dump($input);
echo "Referenced:\n";
var_dump($new_input);

echo "Done";
