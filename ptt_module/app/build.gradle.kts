import java.util.Properties

plugins {
    id("com.android.application")
}

// 💡 LLM API 키는 저장소 루트 .env(git 제외)의 LLM_API_KEY에서 읽음
val secrets = Properties().apply {
    val f = rootProject.file("../.env")
    if (f.exists()) f.inputStream().use { load(it) }
}

android {
    namespace = "com.example.nlp"
    compileSdk = 34

    buildFeatures {
        buildConfig = true
    }

    defaultConfig {
        applicationId = "com.example.nlp"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        buildConfigField("String", "LLM_API_KEY", "\"${secrets.getProperty("LLM_API_KEY", "")}\"")
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.11.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")

    // 💡 학교 LLM API와 통신하기 위한 OkHttp 라이브러리 추가
    implementation("com.squareup.okhttp3:okhttp:4.12.0")

    // ❌ (기존에 있던 TFLite 라이브러리 2줄은 삭제했습니다. 더 이상 쓰지 않으니까요!)

    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.1")
}
