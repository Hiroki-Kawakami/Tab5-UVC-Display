extension IDF {
    class GPIO {
        enum Pin: Int32 {
            case gpio0 = 0
            case gpio1 = 1
            case gpio2 = 2
            case gpio3 = 3
            case gpio4 = 4
            case gpio5 = 5
            case gpio6 = 6
            case gpio7 = 7
            case gpio8 = 8
            case gpio9 = 9
            case gpio10 = 10
            case gpio11 = 11
            case gpio12 = 12
            case gpio13 = 13
            case gpio14 = 14
            case gpio15 = 15
            case gpio16 = 16
            case gpio17 = 17
            case gpio18 = 18
            case gpio19 = 19
            case gpio20 = 20
            case gpio21 = 21
            case gpio22 = 22
            case gpio23 = 23
            case gpio24 = 24
            case gpio25 = 25
            case gpio26 = 26
            case gpio27 = 27
            case gpio28 = 28
            case gpio29 = 29
            case gpio30 = 30
            case gpio31 = 31
            case gpio32 = 32
            case gpio33 = 33
            case gpio34 = 34
            case gpio35 = 35
            case gpio36 = 36
            case gpio37 = 37
            case gpio38 = 38
            case gpio39 = 39
            case gpio40 = 40
            case gpio41 = 41
            case gpio42 = 42
            case gpio43 = 43
            case gpio44 = 44
            case gpio45 = 45
            case gpio46 = 46
            case gpio47 = 47
            case gpio48 = 48
            case gpio49 = 49
            case gpio50 = 50
            case gpio51 = 51
            case gpio52 = 52
            case gpio53 = 53
            case gpio54 = 54

            var value: gpio_num_t {
                return gpio_num_t(rawValue)
            }
        }

        static func reset(pin: Pin) throws(IDF.Error) {
            try IDF.Error.check(gpio_reset_pin(pin.value))
        }
    }
}
