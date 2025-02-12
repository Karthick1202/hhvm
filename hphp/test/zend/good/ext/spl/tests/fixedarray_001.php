<?php
$a = new SplFixedArray(0);
// errors
try {
    $a[0] = "value1";
} catch (RuntimeException $e) {
    echo "Exception: ".$e->getMessage()."\n";
}
try {
    var_dump($a["asdf"]);
} catch (RuntimeException $e) {
    echo "Exception: ".$e->getMessage()."\n";
}
try {
    unset($a[-1]);
} catch (RuntimeException $e) {
    echo "Exception: ".$e->getMessage()."\n";
}
$a->setSize(10);


$a[0] = "value0";
$a[1] = "value1";
$a[2] = "value2";
$a[3] = "value3";
$ref = "value4";
$ref2 =&$ref;
$a[4] = $ref;
$ref = "value5";

unset($a[1]);

var_dump($a[0], $a[2], $a[3], $a[4]);

// countable

var_dump(count($a), $a->getSize(), count($a) == $a->getSize());

// clonable
$b = clone $a;
$a[0] = "valueNew";
var_dump($b[0]);
echo "===DONE===\n";
