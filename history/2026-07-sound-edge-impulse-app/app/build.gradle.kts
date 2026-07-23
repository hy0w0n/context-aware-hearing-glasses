plugins {
    id("com.android.application")
}

android {
    namespace = "com.example.smarthearing_edge"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.example.smarthearing_edge"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        // 🌟 1. C++ 컴파일러 입막음 옵션 강력 적용 (-w -Wno-error)
        externalNativeBuild {
            cmake {
                cppFlags("-std=c++14", "-fexceptions", "-frtti", "-w", "-Wno-error")
                cFlags("-w", "-Wno-error")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    // Gradle에게 설계도 위치 알려주기
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.10.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
}