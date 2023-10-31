; import test

(def b 100)

(def (testf)
(import exporttest)

(assert (bound? 'a))
(assert (= a 3))
(assert (bound? 'f))
(assert (proc? f))

(assert !(bound? 'c))
(assert (= 23 (get-c)))
(inc-c)
(inc-c)
(assert (= 25 (get-c)))
(print (get-c))
)

(assert !(bound? 'a))
(testf)
(assert !(bound? 'a))
(testf)
(assert !(bound? 'a))
;(import test)
(output "import passed\n")

